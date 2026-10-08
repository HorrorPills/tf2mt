// mslbatch — M4 acceptance tool: decode + translate + Metal-compile every shader blob.
//   mslbatch [--scope live|all] [--mode prod|test] [--dump DIR] [--limit N] [--no-compile] [--print] [--link K] <dir|file>...
// --link K: afterwards build K render pipelines from random (VS, PS) pairs (BGRA8 RT0 + Depth32Float_Stencil8),
//           which validates the shared varying interface (msl_abi.h) at link time.
// --scope live (default) = the ADR-001 set (vs_2_0, ps_2_0, ps_2_x, ps_1_1); all = every decodable shader.
// Compiles with MTLMathModeRelaxed (keeps INF/NaN semantics, see ADR-003) and specialises each function with the
// function constants set to non-default values, so both the MSL front end and the specialisation step are exercised.
// Exit status 0 only if every in-scope shader translated and compiled.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../../src/translate/msl.h"
#include "../../src/translate/msl_abi.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace tf2mt;
namespace fs = std::filesystem;

static bool live_scope(const sm::Shader &s)
{
    if (s.stage == sm::Stage::Vertex) return s.major == 2 && s.minor == 0;
    return (s.major == 2 && s.minor <= 1) || (s.major == 1 && s.minor == 1);
}

static std::string first_line(const std::string &e)
{
    // Strip register numbers and token offsets so similar failures group together.
    std::string l = e.substr(0, e.find('\n'));
    std::string out;
    for (size_t i = 0; i < l.size(); i++) {
        if (isdigit((unsigned char)l[i]) && i > 0 && (l[i - 1] == ' ' || isdigit((unsigned char)l[i - 1]) || l[i - 1] == ':')) { if (out.empty() || out.back() != '#') out += '#'; continue; }
        out += l[i];
    }
    return out.substr(0, 200);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        bool all = false, test = false, compile = true, print = false;
        std::string dump;
        size_t limit = SIZE_MAX, link = 0;
        std::vector<fs::path> files;
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--scope" && i + 1 < argc) all = std::string(argv[++i]) == "all";
            else if (a == "--mode" && i + 1 < argc) test = std::string(argv[++i]) == "test";
            else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
            else if (a == "--limit" && i + 1 < argc) limit = strtoull(argv[++i], nullptr, 10);
            else if (a == "--no-compile") compile = false;
            else if (a == "--print") print = true;
            else if (a == "--link" && i + 1 < argc) link = strtoull(argv[++i], nullptr, 10);
            else if (fs::is_directory(a)) { for (auto &e : fs::directory_iterator(a)) if (e.path().extension() == ".bin") files.push_back(e.path()); }
            else files.push_back(a);
        }
        std::sort(files.begin(), files.end());
        if (files.size() > limit) files.resize(limit);
        if (!dump.empty()) fs::create_directories(dump);

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 2; }
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.mathMode = MTLMathModeRelaxed;
        opts.languageVersion = MTLLanguageVersion3_1;
        MTLFunctionConstantValues *fcv = [MTLFunctionConstantValues new];
        int alpha = 5;   // GREATER
        uint32_t clip = 1;
        [fcv setConstantValue:&alpha type:MTLDataTypeInt atIndex:TF2MT_FC_ALPHA_FUNC];
        [fcv setConstantValue:&clip type:MTLDataTypeUInt atIndex:TF2MT_FC_CLIP_MASK];

        std::mutex mu;
        NSMutableArray<id<MTLFunction>> *vs_fns = [NSMutableArray new], *ps_fns = [NSMutableArray new];
        std::map<std::string, std::vector<std::string>> fails;    // grouped error -> files
        std::map<std::string, uint64_t> ok_by_ver, skip_by_ver;
        std::atomic<uint64_t> n_ok{0}, n_fail{0}, n_skip{0}, src_bytes{0}, compile_us{0};
        std::atomic<size_t> done{0};
        auto t0 = std::chrono::steady_clock::now();
        msl::Options mo;
        mo.mode = test ? msl::Mode::ComputeTest : msl::Mode::Production;

        auto work = [&](size_t idx) {
            @autoreleasepool {
                const fs::path &p = files[idx];
                std::ifstream f(p, std::ios::binary);
                std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
                std::vector<uint32_t> w(b.size() / 4);
                memcpy(w.data(), b.data(), w.size() * 4);
                sm::Shader s;
                std::string err, ver = "?";
                msl::Output out;
                auto record_fail = [&](const std::string &kind, const std::string &e, const std::string *source) {
                    n_fail++;
                    std::lock_guard<std::mutex> g(mu);
                    auto &v = fails[ver + " " + kind + ": " + first_line(e)];
                    v.push_back(p.filename().string());
                    if (!dump.empty() && v.size() <= 3) {
                        std::ofstream d(fs::path(dump) / (p.stem().string() + ".txt"));
                        d << "// " << e << "\n";
                        if (source) d << *source;
                    }
                };
                if (!sm::decode(w.data(), w.size(), s, err)) { record_fail("decode", err, nullptr); return; }
                ver = sm::version_string(s);
                if (!all && !live_scope(s)) { n_skip++; std::lock_guard<std::mutex> g(mu); skip_by_ver[ver]++; return; }
                if (!msl::translate(s, mo, out, err)) { record_fail("translate", err, nullptr); return; }
                src_bytes += out.source.size();
                if (print) { std::lock_guard<std::mutex> g(mu); fputs(out.source.c_str() + out.source.find("// translated"), stdout); }
                if (compile) {
                    auto c0 = std::chrono::steady_clock::now();
                    NSError *nerr = nil;
                    NSString *src = [[NSString alloc] initWithBytes:out.source.data() length:out.source.size() encoding:NSUTF8StringEncoding];
                    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opts error:&nerr];
                    if (!lib) { record_fail("compile", nerr ? nerr.localizedDescription.UTF8String : "nil library", &out.source); return; }
                    id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:out.entry.c_str()] constantValues:fcv error:&nerr];
                    if (!fn) { record_fail("specialise", nerr ? nerr.localizedDescription.UTF8String : "nil function", &out.source); return; }
                    if (link) { std::lock_guard<std::mutex> g(mu); [(s.stage == sm::Stage::Vertex ? vs_fns : ps_fns) addObject:fn]; }
                    compile_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - c0).count();
                }
                n_ok++;
                {
                    std::lock_guard<std::mutex> g(mu);
                    ok_by_ver[ver]++;
                }
                size_t d = ++done;
                if (d % 20000 == 0) fprintf(stderr, "  %zu ok...\n", d);
            }
        };
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); t++)
            pool.emplace_back([&] { for (size_t i; (i = next++) < files.size();) work(i); });
        for (auto &t : pool) t.join();

        uint64_t link_ok = 0, link_fail = 0;
        if (link && vs_fns.count && ps_fns.count) {
            std::mt19937 rng(1234);
            for (size_t k = 0; k < link; k++) {
                @autoreleasepool {
                    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
                    d.vertexFunction = vs_fns[rng() % vs_fns.count];
                    d.fragmentFunction = ps_fns[rng() % ps_fns.count];
                    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                    d.depthAttachmentPixelFormat = d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
                    NSError *e = nil;
                    if ([dev newRenderPipelineStateWithDescriptor:d error:&e]) link_ok++;
                    else if (link_fail++ < 5) printf("LINK FAIL %s\n", e.localizedDescription.UTF8String);
                }
            }
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("files %zu  ok %llu  fail %llu  out-of-scope %llu  (%.1f s wall", files.size(), (unsigned long long)n_ok.load(),
               (unsigned long long)n_fail.load(), (unsigned long long)n_skip.load(), secs);
        if (compile && n_ok) printf(", mean compile %.2f ms/shader/thread", compile_us.load() / 1000.0 / n_ok.load());
        if (n_ok) printf(", mean MSL %llu bytes", (unsigned long long)(src_bytes.load() / n_ok.load()));
        printf(")\n");
        if (link) printf("link: %llu/%llu render pipelines created\n", (unsigned long long)link_ok, (unsigned long long)(link_ok + link_fail));
        for (auto &[v, n] : ok_by_ver) printf("  ok   %-8s %llu\n", v.c_str(), (unsigned long long)n);
        for (auto &[v, n] : skip_by_ver) printf("  skip %-8s %llu\n", v.c_str(), (unsigned long long)n);
        for (auto &[k, v] : fails) printf("FAIL x%zu  %s  (e.g. %s)\n", v.size(), k.c_str(), v[0].c_str());
        return (n_fail || link_fail) ? 1 : 0;
    }
}
