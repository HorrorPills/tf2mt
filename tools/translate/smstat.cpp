// smstat — decode every shader blob under the given directories with the tf2mt decoder and print feature counts.
//   smstat <dir|file>... [--sm2]      (--sm2: only vs_2_*/ps_2_* plus ps_1_*, the ADR-001 scope)
// Doubles as a decoder robustness test: any decode failure is listed with its file and message.
#include "../../src/translate/sm.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

using namespace tf2mt::sm;
namespace fs = std::filesystem;

struct Stats {
    std::map<std::string, uint64_t> c;
    std::vector<std::string> failures;
    void add(const std::string &k, uint64_t n = 1) { c[k] += n; }
    void merge(const Stats &o) { for (auto &[k, v] : o.c) c[k] += v; failures.insert(failures.end(), o.failures.begin(), o.failures.end()); }
};

static bool in_scope(const Shader &s) { return s.major == 2 || (s.major == 1 && s.stage == Stage::Pixel); }

static void scan(const fs::path &p, bool sm2, Stats &st)
{
    std::ifstream f(p, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> w(b.size() / 4);
    memcpy(w.data(), b.data(), w.size() * 4);
    Shader s; std::string err;
    if (!decode(w.data(), w.size(), s, err)) { st.failures.push_back(p.filename().string() + ": " + err); return; }
    if (sm2 && !in_scope(s)) { st.add("skip " + version_string(s)); return; }
    std::string v = version_string(s), k = s.stage == Stage::Pixel ? "ps" : "vs";
    st.add("ver " + v);
    uint32_t maxc = 0, maxr = 0;
    for (auto &in : s.instrs) {
        char ob[64];
        snprintf(ob, sizeof ob, "op %s %-8s ctl=%u", k.c_str(), op_name(in.op), in.control);
        st.add(ob);
        if (in.predicated) st.add("feat " + k + " predicated");
        if (in.coissue) st.add("feat " + k + " coissue");
        if (in.has_dst) {
            snprintf(ob, sizeof ob, "dst %s type=%u", k.c_str(), in.dst.type); st.add(ob);
            if (in.dst.saturate) st.add("dstmod " + k + " sat");
            if (in.dst.partial) st.add("dstmod " + k + " pp");
            if (in.dst.centroid) st.add("dstmod " + k + " centroid");
            if (in.dst.shift) { snprintf(ob, sizeof ob, "dstmod %s shift=%d", k.c_str(), in.dst.shift); st.add(ob); }
            if (in.dst.type == TEMP) maxr = std::max(maxr, in.dst.index);
            if (in.op == DCL) {
                snprintf(ob, sizeof ob, "dcl %s type=%u usage=%u idx=%u tex=%u", k.c_str(), in.dst.type, in.dcl_usage,
                         in.dcl_usage_index, in.dcl_tex);
                st.add(ob);
            }
        }
        for (int j = 0; j < in.nsrc; j++) {
            auto &sr = in.src[j];
            snprintf(ob, sizeof ob, "src %s type=%u", k.c_str(), sr.type); st.add(ob);
            if (sr.mod) { snprintf(ob, sizeof ob, "srcmod %s %s %u", k.c_str(), op_name(in.op), sr.mod); st.add(ob); }
            if (sr.relative) { snprintf(ob, sizeof ob, "rel %s type=%u reltype=%u comp=%u", k.c_str(), sr.type, sr.rel_type, sr.rel_comp); st.add(ob); }
            if (sr.type == CONST) maxc = std::max(maxc, sr.index);
            if (sr.type == TEMP) maxr = std::max(maxr, sr.index);
        }
    }
    char mb[64];
    snprintf(mb, sizeof mb, "max %s const>=%u", k.c_str(), maxc / 32 * 32); st.add(mb);
    snprintf(mb, sizeof mb, "max %s temp=%u", k.c_str(), maxr); st.add(mb);
    snprintf(mb, sizeof mb, "len %s instrs>=%zu", k.c_str(), s.instrs.size() / 32 * 32); st.add(mb);
}

int main(int argc, char **argv)
{
    std::vector<fs::path> files;
    bool sm2 = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--sm2") { sm2 = true; continue; }
        if (fs::is_directory(a)) {
            for (auto &e : fs::directory_iterator(a))
                if (e.path().extension() == ".bin") files.push_back(e.path());
        } else files.push_back(a);
    }
    unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    std::vector<Stats> parts(nt);
    std::atomic<size_t> next{0};
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; t++)
        th.emplace_back([&, t] { for (size_t i; (i = next++) < files.size();) scan(files[i], sm2, parts[t]); });
    for (auto &x : th) x.join();
    Stats all;
    for (auto &p : parts) all.merge(p);
    printf("files %zu  decode-failures %zu\n", files.size(), all.failures.size());
    for (auto &[k, v] : all.c) printf("%-60s %llu\n", k.c_str(), (unsigned long long)v);
    for (size_t i = 0; i < all.failures.size() && i < 50; i++) printf("FAIL %s\n", all.failures[i].c_str());
    return all.failures.empty() ? 0 : 1;
}
