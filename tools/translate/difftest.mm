// difftest — differential test of the shader translator: GPU (translated MSL, Mode::ComputeTest) vs the CPU reference
// interpreter (src/translate/interp.cpp) on random inputs and constants.
//   difftest [--scope live|all] [--n N] [--every K] [--verbose] <dir|file>...
// Each shader runs N invocations (default 64) with deterministic per-shader random inputs; textures are 1x1 constant
// colours (sampling plumbing and swizzles are tested, filtering is not). Exit 0 only if every shader matches.
#include "harness.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace tf2mt;
namespace fs = std::filesystem;

static bool live_scope(const sm::Shader &s)
{
    if (s.stage == sm::Stage::Vertex) return s.major == 2 && s.minor == 0;
    return (s.major == 2 && s.minor <= 1) || (s.major == 1 && s.minor == 1);
}

// Values agree when both are NaN, equal infinities, or within a tolerance that covers Metal's relaxed-math
// transcendental precision (exp2/log2/rsqrt/sin/cos a few ulp; differences propagate through later arithmetic).
static bool close(float g, float c)
{
    if (std::isnan(g) || std::isnan(c)) return std::isnan(g) && std::isnan(c);
    if (std::isinf(g) || std::isinf(c)) return g == c;
    float d = std::fabs(g - c);
    return d <= 2e-3f + 2e-3f * std::fabs(c);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        bool all = false, verbose = false;
        unsigned n = 64, every = 1;
        std::vector<fs::path> files;
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--scope" && i + 1 < argc) all = std::string(argv[++i]) == "all";
            else if (a == "--n" && i + 1 < argc) n = unsigned(atoi(argv[++i]));
            else if (a == "--every" && i + 1 < argc) every = unsigned(atoi(argv[++i]));
            else if (a == "--verbose") verbose = true;
            else if (fs::is_directory(a)) { for (auto &e : fs::directory_iterator(a)) if (e.path().extension() == ".bin") files.push_back(e.path()); }
            else files.push_back(a);
        }
        std::sort(files.begin(), files.end());
        if (every > 1) { std::vector<fs::path> f; for (size_t i = 0; i < files.size(); i += every) f.push_back(files[i]); files.swap(f); }

        harness::Gpu gpu_run;

        std::mutex mu;
        std::map<std::string, uint64_t> by_ver_ok, by_ver_bad;
        std::vector<std::string> reports;
        std::atomic<uint64_t> n_ok{0}, n_bad{0}, n_err{0}, n_skip{0};

        auto work = [&](size_t idx) {
            @autoreleasepool {
                const fs::path &p = files[idx];
                std::ifstream f(p, std::ios::binary);
                std::vector<char> raw((std::istreambuf_iterator<char>(f)), {});
                std::vector<uint32_t> w(raw.size() / 4);
                memcpy(w.data(), raw.data(), w.size() * 4);
                sm::Shader s;
                std::string err;
                auto report = [&](const std::string &m) { std::lock_guard<std::mutex> g(mu); reports.push_back(p.filename().string() + ": " + m); };
                if (!sm::decode(w.data(), w.size(), s, err)) { n_err++; report("decode: " + err); return; }
                if (!all && !live_scope(s)) { n_skip++; return; }
                std::string ver = sm::version_string(s);
                msl::Options mo;
                mo.mode = msl::Mode::ComputeTest;
                msl::Output out;
                if (!msl::translate(s, mo, out, err)) { n_err++; report("translate: " + err); return; }
                // deterministic per-shader random environment
                std::mt19937 rng(uint32_t(std::hash<std::string>{}(p.filename().string())));
                std::uniform_real_distribution<float> u(-2.0f, 2.0f), u01(0.0f, 1.0f);
                auto rnd = [&]() { float k = u01(rng); return k < 0.08f ? 0.0f : k < 0.12f ? 1.0f : u(rng); };
                const unsigned NC = 256;
                std::vector<ref::Vec4> consts(NC);
                for (auto &c : consts) for (float &x : c.v) x = rnd();
                ref::Env env;
                env.c = consts.data();
                for (int k = 0; k < 16; k++) env.i[k][0] = int(rng() % 5);
                env.b = rng();
                for (int k = 0; k < 16; k++) for (float &x : env.tex[k].v) x = u01(rng);
                std::vector<ref::Vec4> in(n * TF2MT_TEST_STRIDE), gpu(n * TF2MT_TEST_STRIDE), cpu(n * TF2MT_TEST_STRIDE);
                for (unsigned t = 0; t < n; t++)
                    for (unsigned k = 0; k < TF2MT_TEST_STRIDE; k++)
                        for (int j = 0; j < 4; j++) {
                            float x = s.stage == sm::Stage::Vertex ? rnd()
                                    : (k == TF2MT_VAR_C0 || k == TF2MT_VAR_C1) ? u01(rng) : u(rng);
                            in[t * TF2MT_TEST_STRIDE + k].v[j] = x;
                        }

                if (!gpu_run.run(out, env, in.data(), n, gpu.data(), err)) { n_err++; report(err); return; }

                unsigned bad_inv = 0;
                std::string first;
                for (unsigned t = 0; t < n; t++) {
                    ref::Vec4 *co = &cpu[t * TF2MT_TEST_STRIDE];
                    if (!ref::run(s, env, &in[t * TF2MT_TEST_STRIDE], co, err)) { n_err++; report(err); return; }
                    bool inv_bad = false;
                    for (unsigned k = 0; k < TF2MT_TEST_STRIDE && !inv_bad; k++)
                        for (int j = 0; j < 4; j++) {
                            float g = gpu[t * TF2MT_TEST_STRIDE + k].v[j], c = co[k].v[j];
                            if (!close(g, c)) {
                                inv_bad = true;
                                if (first.empty()) {
                                    char b[160];
                                    snprintf(b, sizeof b, "inv %u slot %u.%c gpu %.9g cpu %.9g", t, k, "xyzw"[j], g, c);
                                    first = b;
                                }
                                break;
                            }
                        }
                    bad_inv += inv_bad;
                }
                std::lock_guard<std::mutex> g(mu);
                if (bad_inv) {
                    n_bad++; by_ver_bad[ver]++;
                    char b[64];
                    snprintf(b, sizeof b, " (%u/%u invocations)", bad_inv, n);
                    reports.push_back(p.filename().string() + " " + ver + ": " + first + b);
                } else { n_ok++; by_ver_ok[ver]++; }
            }
        };
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); t++)
            pool.emplace_back([&] { for (size_t i; (i = next++) < files.size();) work(i); });
        for (auto &t : pool) t.join();

        printf("shaders %zu  match %llu  mismatch %llu  error %llu  out-of-scope %llu  (%u invocations each)\n", files.size(),
               (unsigned long long)n_ok.load(), (unsigned long long)n_bad.load(), (unsigned long long)n_err.load(),
               (unsigned long long)n_skip.load(), n);
        for (auto &[v, c] : by_ver_ok) printf("  match    %-8s %llu\n", v.c_str(), (unsigned long long)c);
        for (auto &[v, c] : by_ver_bad) printf("  mismatch %-8s %llu\n", v.c_str(), (unsigned long long)c);
        std::sort(reports.begin(), reports.end());
        for (size_t i = 0; i < reports.size() && (verbose || i < 40); i++) printf("  %s\n", reports[i].c_str());
        return (n_bad || n_err) ? 1 : 0;
    }
}
