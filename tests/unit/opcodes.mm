// Golden opcode tests for the shader translator (M4 acceptance: "unit/golden opcode tests green").
// Each case is a hand-assembled D3D9 shader + hand-computed expected outputs (docs/semantics.md). Every case runs on
// the GPU (translated MSL, compute test mode) AND on the CPU reference interpreter; both must match the golden values.
#include "../../tools/translate/harness.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <vector>

using namespace tf2mt;

namespace {

enum : unsigned { TEMP = 0, INPUT = 1, CONST = 2, ADDR = 3, TEX = 3, RAST = 4, ATTR = 5, TCOUT = 6, CINT = 7, COUT = 8,
                  SAMP = 10, CBOOL = 14 };
const float INF = INFINITY;

uint32_t reg(unsigned type, unsigned idx) { return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) | idx; }
uint32_t mask(const char *m)
{
    uint32_t r = 0;
    for (; *m; m++) r |= 1u << (strchr("xyzw", *m) - "xyzw");
    return r;
}
// dst: mask string, modifier bits (1 = sat), shift
uint32_t D(unsigned type, unsigned idx, const char *m = "xyzw", unsigned mod = 0, int shift = 0)
{
    return reg(type, idx) | mask(m) << 16 | mod << 20 | (uint32_t(shift) & 0xfu) << 24;
}
// src: swizzle string (1 char = replicate), modifier (1 neg, 2 bias, 4 bx2, 6 comp, 11 abs)
uint32_t S(unsigned type, unsigned idx, const char *sw = "xyzw", unsigned mod = 0)
{
    uint32_t s = 0;
    size_t n = strlen(sw);
    for (int i = 0; i < 4; i++) s |= uint32_t(strchr("xyzw", sw[i < int(n) ? i : int(n) - 1]) - "xyzw") << (2 * i);
    return reg(type, idx) | s << 16 | mod << 24;
}
uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

struct Asm {
    std::vector<uint32_t> t;
    int major;
    Asm(bool vs, int maj, int min) : major(maj) { t.push_back((vs ? 0xfffe0000u : 0xffff0000u) | maj << 8 | min); }
    Asm &op(uint16_t o, std::initializer_list<uint32_t> p, uint32_t ctl = 0, bool coissue = false)
    {
        t.push_back(o | ctl << 16 | (major >= 2 ? uint32_t(p.size()) << 24 : 0) | (coissue ? 1u << 30 : 0));
        t.insert(t.end(), p);
        return *this;
    }
    Asm &def(unsigned idx, float x, float y, float z, float w) { return op(81, {D(CONST, idx), fbits(x), fbits(y), fbits(z), fbits(w)}); }
    Asm &defi(unsigned idx, int x) { return op(48, {D(CINT, idx), uint32_t(x), 0, 0, 0}); }
    Asm &defb(unsigned idx, bool b) { return op(47, {D(CBOOL, idx), b ? 1u : 0u}); }
    Asm &dcl(unsigned usage, unsigned uidx, unsigned textype, uint32_t dst) { return op(31, {0x80000000u | usage | uidx << 16 | textype << 27, dst}); }
    // source with relative addressing through a0.<comp> (SM2: extra token)
    Asm &op_rel(uint16_t o, uint32_t dst, uint32_t src, char comp)
    {
        uint32_t c = uint32_t(strchr("xyzw", comp) - "xyzw");
        return op(o, {dst, src | 1u << 13, reg(ADDR, 0) | (c * 0x55u) << 16});
    }
    std::vector<uint32_t> end() { t.push_back(0x0000ffffu); return t; }
};

enum Op : uint16_t { MOV = 1, ADD = 2, MAD = 4, MUL = 5, RCP = 6, RSQ = 7, DP3 = 8, DP4 = 9, MIN = 10, MAX = 11, SLT = 12,
                     SGE = 13, EXP = 14, LOG = 15, LRP = 18, FRC = 19, POW = 32, NRM = 36, SINCOS = 37, REP = 38,
                     ENDREP = 39, IF = 40, ELSE = 42, ENDIF = 43, MOVA = 46, TEXCOORD = 64, TEXKILL = 65, TEXLD = 66,
                     CND = 80, CMP = 88, DP2ADD = 90 };

struct Check { unsigned slot; const char *m; ref::Vec4 v; };
struct Inv { std::map<unsigned, ref::Vec4> in; std::vector<Check> expect; };
struct Case {
    const char *name;
    std::vector<uint32_t> tokens;
    std::vector<Inv> invs;
    std::function<void(ref::Env &, ref::Vec4 *)> setup = nullptr;   // optional constants / textures
};

bool close(float g, float e)
{
    if (std::isnan(e)) return std::isnan(g);
    if (std::isinf(e)) return g == e;
    return std::fabs(g - e) <= 1e-5f + 1e-4f * std::fabs(e);
}

// ps_2_0 constants shared by the pixel cases
Asm ps_common(int minor = 0)
{
    Asm a(false, 2, minor);
    a.def(20, 0.0f, -0.0f, 1.0f, -1.0f).def(21, INF, -INF, 2.0f, 0.5f).def(22, 3, 4, 0, 2).def(23, -0.25f, 1.5f, -8, 0.25f)
     .def(24, -2, 0.5f, 4, 0).def(25, 0, 1, 2, 0).def(26, INF, 3, 4, 0).def(27, 1, 1, 1, 0).def(28, 1, 2, 3, INF)
     .def(29, 0.25f, 0.5f, 1, 0);
    return a;
}
uint32_t oC(unsigned n) { return D(COUT, n); }

std::vector<Case> cases()
{
    std::vector<Case> v;
    const float NaN = NAN;
    // ---------------- pixel shader arithmetic (ps_2_0)
    v.push_back({"rcp: 0 and -0 -> +INF, exact reciprocal",
        ps_common().op(RCP, {D(TEMP, 0, "x"), S(CONST, 20, "x")}).op(RCP, {D(TEMP, 0, "y"), S(CONST, 20, "y")})
            .op(RCP, {D(TEMP, 0, "z"), S(CONST, 21, "z")}).op(RCP, {D(TEMP, 0, "w"), S(CONST, 20, "w")})
            .op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{INF, INF, 0.5f, -1}}}}}}});
    v.push_back({"rsq: |x|, rsq(0) = +INF",
        ps_common().op(RSQ, {D(TEMP, 0, "x"), S(CONST, 20, "x")}).op(RSQ, {D(TEMP, 0, "y"), S(CONST, 23, "z")})
            .op(RSQ, {D(TEMP, 0, "z"), S(CONST, 20, "z")}).op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyz", {{INF, 0.353553391f, 1, 0}}}}}}});
    v.push_back({"log/exp: log(0) = -INF, log uses |x|, exp(-INF) = 0",
        ps_common().op(LOG, {D(TEMP, 0, "x"), S(CONST, 20, "x")}).op(LOG, {D(TEMP, 0, "y"), S(CONST, 23, "z")})
            .op(EXP, {D(TEMP, 0, "z"), S(CONST, 20, "x")}).op(EXP, {D(TEMP, 0, "w"), S(CONST, 21, "y")})
            .op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{-INF, 3, 1, 0}}}}}}});
    v.push_back({"pow: pow(0,0) = 1, |x| base, pow(0,2) = 0",
        ps_common().op(POW, {D(TEMP, 0, "x"), S(CONST, 20, "x"), S(CONST, 20, "x")})
            .op(POW, {D(TEMP, 0, "y"), S(CONST, 24, "x"), S(CONST, 21, "z")})
            .op(POW, {D(TEMP, 0, "z"), S(CONST, 20, "x"), S(CONST, 21, "z")})
            .op(POW, {D(TEMP, 0, "w"), S(CONST, 24, "z"), S(CONST, 21, "w")}).op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{1, 4, 0, 2}}}}}}});
    v.push_back({"mul: legacy 0 * INF = 0",
        ps_common().op(MUL, {D(TEMP, 0), S(CONST, 20), S(CONST, 21)}).op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{0, 0, 2, -0.5f}}}}}}});
    v.push_back({"mad: legacy 0 * INF + c",
        ps_common().op(MAD, {D(TEMP, 0), S(CONST, 20), S(CONST, 21), S(CONST, 22)}).op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{3, 4, 2, 1.5f}}}}}}});
    v.push_back({"dp3/dp4: legacy products, replicated result",
        ps_common().op(DP3, {D(TEMP, 0), S(CONST, 25), S(CONST, 26)}).op(DP4, {D(TEMP, 1), S(CONST, 27), S(CONST, 28)})
            .op(MOV, {oC(0), S(TEMP, 0)}).op(MOV, {oC(1), S(TEMP, 1)}).end(),
        {{{}, {{0, "xyzw", {{11, 11, 11, 11}}}, {1, "xyzw", {{6, 6, 6, 6}}}}}}});
    v.push_back({"dp2add",
        ps_common().op(DP2ADD, {D(TEMP, 0), S(CONST, 22), S(CONST, 29), S(CONST, 24, "z")}).op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{6.75f, 6.75f, 6.75f, 6.75f}}}}}}});
    v.push_back({"nrm: unit vector (w scaled too), zero vector -> 0",
        ps_common().op(NRM, {D(TEMP, 0), S(CONST, 22)}).op(NRM, {D(TEMP, 1), S(CONST, 20, "x")})
            .op(MOV, {oC(0), S(TEMP, 0)}).op(MOV, {oC(1), S(TEMP, 1)}).end(),
        {{{}, {{0, "xyzw", {{0.6f, 0.8f, 0, 0.4f}}}, {1, "xyzw", {{0, 0, 0, 0}}}}}}});
    v.push_back({"cmp: src0 >= 0 (including -0)",
        ps_common().op(CMP, {D(TEMP, 0), S(CONST, 20), S(CONST, 22), S(CONST, 24)}).op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{3, 4, 0, 0}}}}}}});
    v.push_back({"lrp: s2 + s0 * (s1 - s2)",
        ps_common().op(LRP, {D(TEMP, 0), S(CONST, 29), S(CONST, 22), S(CONST, 24)}).op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{-0.75f, 2.25f, 0, 0}}}}}}});
    v.push_back({"frc: x - floor(x)",
        ps_common().op(FRC, {D(TEMP, 0), S(CONST, 23)}).op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{0.75f, 0.5f, 0, 0.25f}}}}}}});
    v.push_back({"min/max",
        ps_common().op(MIN, {D(TEMP, 0), S(CONST, 20), S(CONST, 23)}).op(MAX, {D(TEMP, 1), S(CONST, 20), S(CONST, 23)})
            .op(MOV, {oC(0), S(TEMP, 0)}).op(MOV, {oC(1), S(TEMP, 1)}).end(),
        {{{}, {{0, "xyzw", {{-0.25f, 0, -8, -1}}}, {1, "xyzw", {{0, 1.5f, 1, 0.25f}}}}}}});
    v.push_back({"_sat: clamps, NaN -> 0; unsaturated NaN propagates",
        ps_common().op(ADD, {D(TEMP, 0), S(CONST, 21, "x"), S(CONST, 21, "y")}).op(MOV, {D(COUT, 0, "xyzw", 1), S(TEMP, 0)})
            .op(MOV, {D(TEMP, 1, "xyzw", 1), S(CONST, 24)}).op(MOV, {oC(1), S(TEMP, 1)}).op(MOV, {oC(2), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{0, 0, 0, 0}}}, {1, "xyzw", {{0, 0.5f, 1, 0}}}, {2, "x", {{NaN, 0, 0, 0}}}}}}});
    v.push_back({"source modifiers: neg, abs, swizzle",
        ps_common(1).op(ADD, {D(TEMP, 0), S(CONST, 22, "xyzw", 1), S(CONST, 24, "wzyx", 11)}).op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{-3, -0, 0.5f, 0}}}}}}});
    v.push_back({"write mask leaves other components",
        ps_common().op(MOV, {D(TEMP, 0), S(CONST, 22)}).op(MOV, {D(TEMP, 0, "yw"), S(CONST, 24)}).op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{}, {{0, "xyzw", {{3, 0.5f, 0, 0}}}}}}});
    // ---------------- textures and texkill (ps_2_x)
    auto tex_setup = [](ref::Env &e, ref::Vec4 *) { e.tex[0] = {{0.1f, 0.2f, 0.3f, 0.4f}}; e.tex[3] = {{0.5f, 0.6f, 0.7f, 0.8f}}; };
    v.push_back({"texld / texldp / sampler swizzle / cube",
        ps_common(1).dcl(5, 0, 0, D(TEX, 0)).dcl(0, 0, 2, D(SAMP, 0)).dcl(0, 0, 3, D(SAMP, 3))
            .op(TEXLD, {D(TEMP, 0), S(TEX, 0), S(SAMP, 0)}).op(TEXLD, {D(TEMP, 1), S(TEX, 0), S(SAMP, 0, "wzyx")}, 0)
            .op(TEXLD, {D(TEMP, 2), S(TEX, 0), S(SAMP, 0)}, 1).op(TEXLD, {D(TEMP, 3), S(TEX, 0), S(SAMP, 3)})
            .op(MOV, {oC(0), S(TEMP, 0)}).op(MOV, {oC(1), S(TEMP, 1)}).op(MOV, {oC(2), S(TEMP, 2)}).op(MOV, {oC(3), S(TEMP, 3)}).end(),
        {{{{3, {{0.5f, 0.5f, 0.5f, 2}}}},
          {{0, "xyzw", {{0.1f, 0.2f, 0.3f, 0.4f}}}, {1, "xyzw", {{0.4f, 0.3f, 0.2f, 0.1f}}},
           {2, "xyzw", {{0.1f, 0.2f, 0.3f, 0.4f}}}, {3, "xyzw", {{0.5f, 0.6f, 0.7f, 0.8f}}}}}},
        tex_setup});
    v.push_back({"texkill: masked components < 0",
        ps_common().dcl(5, 0, 0, D(TEX, 0)).op(MOV, {D(TEMP, 0), S(TEX, 0)}).op(TEXKILL, {D(TEMP, 0, "xyz")})
            .op(MOV, {oC(0), S(TEMP, 0)}).end(),
        {{{{3, {{1, 1, 1, -1}}}}, {{5, "x", {{0, 0, 0, 0}}}}},
         {{{3, {{1, -0.5f, 1, 1}}}}, {{5, "x", {{1, 0, 0, 0}}}}}}});
    // ---------------- vertex shaders (vs_2_0)
    auto vs_consts = [](ref::Env &, ref::Vec4 *c) {
        for (int i = 0; i < 256; i++) c[i] = {{float(i), float(i), float(i), float(i)}};
    };
    v.push_back({"mova (round to nearest) + relative constants, def override, out-of-range -> 0",
        Asm(true, 2, 0).def(11, 7, 7, 7, 7).dcl(0, 0, 0, D(INPUT, 0))
            .op(MOVA, {D(ADDR, 0, "x"), S(INPUT, 0, "x")}).op_rel(MOV, D(RAST, 0), S(CONST, 10), 'x').end(),
        {{{{0, {{1.5f, 0, 0, 0}}}}, {{0, "xyzw", {{12, 12, 12, 12}}}}},
         {{{0, {{-0.5f, 0, 0, 0}}}}, {{0, "xyzw", {{10, 10, 10, 10}}}}},
         {{{0, {{0.49f, 0, 0, 0}}}}, {{0, "xyzw", {{10, 10, 10, 10}}}}},
         {{{0, {{1, 0, 0, 0}}}}, {{0, "xyzw", {{7, 7, 7, 7}}}}},
         {{{0, {{300, 0, 0, 0}}}}, {{0, "xyzw", {{0, 0, 0, 0}}}}},
         {{{0, {{-20, 0, 0, 0}}}}, {{0, "xyzw", {{0, 0, 0, 0}}}}}},
        vs_consts});
    v.push_back({"sincos (vs_2_0 three-operand form)",
        Asm(true, 2, 0).dcl(0, 0, 0, D(INPUT, 0)).op(MOV, {D(TEMP, 0), S(INPUT, 0, "y")})
            .op(SINCOS, {D(TEMP, 0, "xy"), S(INPUT, 0, "x"), S(CONST, 0), S(CONST, 1)}).op(MOV, {D(TCOUT, 0), S(TEMP, 0)}).end(),
        {{{{0, {{0, 5, 0, 0}}}}, {{3, "xyzw", {{1, 0, 5, 5}}}}},
         {{{0, {{1.57079633f, 5, 0, 0}}}}, {{3, "xy", {{0, 1, 0, 0}}}}}}});
    v.push_back({"slt/sge",
        Asm(true, 2, 0).def(20, 0, -0.0f, 1, -1).dcl(0, 0, 0, D(INPUT, 0))
            .op(SLT, {D(TCOUT, 0), S(INPUT, 0), S(CONST, 20)}).op(SGE, {D(TCOUT, 1), S(INPUT, 0), S(CONST, 20)}).end(),
        {{{{0, {{-1, 0, 1, 2}}}}, {{3, "xyzw", {{1, 0, 0, 0}}}, {4, "xyzw", {{0, 1, 1, 1}}}}}}});
    v.push_back({"rep: count from i#.x, defi override, negative count -> 0 iterations",
        Asm(true, 2, 0).def(20, 0, 1, 0, 0).defi(1, 2)
            .op(MOV, {D(TEMP, 0), S(CONST, 20, "x")}).op(REP, {S(CINT, 0)}).op(ADD, {D(TEMP, 0), S(TEMP, 0), S(CONST, 20, "y")}).op(ENDREP, {})
            .op(MOV, {D(TEMP, 1), S(CONST, 20, "x")}).op(REP, {S(CINT, 1)}).op(ADD, {D(TEMP, 1), S(TEMP, 1), S(CONST, 20, "y")}).op(ENDREP, {})
            .op(MOV, {D(TEMP, 2), S(CONST, 20, "x")}).op(REP, {S(CINT, 2)}).op(ADD, {D(TEMP, 2), S(TEMP, 2), S(CONST, 20, "y")}).op(ENDREP, {})
            .op(MOV, {D(TCOUT, 0), S(TEMP, 0)}).op(MOV, {D(TCOUT, 1), S(TEMP, 1)}).op(MOV, {D(TCOUT, 2), S(TEMP, 2)}).end(),
        {{{}, {{3, "x", {{3, 0, 0, 0}}}, {4, "x", {{2, 0, 0, 0}}}, {5, "x", {{0, 0, 0, 0}}}}}},
        [](ref::Env &e, ref::Vec4 *) { e.i[0][0] = 3; e.i[1][0] = 9; e.i[2][0] = -5; }});
    v.push_back({"if b# / else, defb override",
        Asm(true, 2, 0).def(20, 1, 2, 0, 0).defb(2, true)
            .op(IF, {S(CBOOL, 0)}).op(MOV, {D(TCOUT, 0), S(CONST, 20, "x")}).op(ELSE, {}).op(MOV, {D(TCOUT, 0), S(CONST, 20, "y")}).op(ENDIF, {})
            .op(IF, {S(CBOOL, 1)}).op(MOV, {D(TCOUT, 1), S(CONST, 20, "x")}).op(ELSE, {}).op(MOV, {D(TCOUT, 1), S(CONST, 20, "y")}).op(ENDIF, {})
            .op(IF, {S(CBOOL, 2)}).op(MOV, {D(TCOUT, 2), S(CONST, 20, "x")}).op(ENDIF, {}).end(),
        {{{}, {{3, "x", {{1, 0, 0, 0}}}, {4, "x", {{2, 0, 0, 0}}}, {5, "x", {{1, 0, 0, 0}}}}}},
        [](ref::Env &e, ref::Vec4 *) { e.b = 0x1; }});
    v.push_back({"vs_2_0 colour outputs saturate; texcoords do not; unwritten outputs = (0,0,0,1)",
        Asm(true, 2, 0).def(24, -2, 0.5f, 4, 0).op(MOV, {D(ATTR, 0), S(CONST, 24)}).op(MOV, {D(TCOUT, 0), S(CONST, 24)}).end(),
        {{{}, {{1, "xyzw", {{0, 0.5f, 1, 0}}}, {3, "xyzw", {{-2, 0.5f, 4, 0}}}, {4, "xyzw", {{0, 0, 0, 1}}}}}}});
    // ---------------- ps_1_1
    v.push_back({"ps_1_1: constants clamp to [-1,1]; r0 is the output",
        Asm(false, 1, 1).def(1, 2, -3, 0.5f, 1).op(MOV, {D(TEMP, 0), S(CONST, 1)}).end(),
        {{{}, {{0, "xyzw", {{1, -1, 0.5f, 1}}}}}}});
    v.push_back({"ps_1_1: _bias, _bx2, 1-x source modifiers",
        Asm(false, 1, 1).def(0, 0.25f, 0.75f, 1, 0)
            .op(ADD, {D(TEMP, 1), S(CONST, 0, "xyzw", 2), S(CONST, 0, "xyzw", 4)}).op(ADD, {D(TEMP, 0), S(TEMP, 1), S(CONST, 0, "xyzw", 6)}).end(),
        {{{}, {{0, "xyzw", {{0, 1, 1.5f, -0.5f}}}}}}});
    v.push_back({"ps_1_1: _x2 shift and co-issue reads before writes",
        Asm(false, 1, 1).def(0, 0.25f, 0.75f, 1, 0).def(1, 2, -3, 0.5f, 1).def(3, 0.1f, 0.2f, 0.3f, 0.4f)
            .op(MOV, {D(TEMP, 0), S(CONST, 3)})
            .op(MUL, {D(TEMP, 0, "xyz", 0, 1), S(CONST, 0), S(CONST, 1)}).op(MOV, {D(TEMP, 0, "w"), S(TEMP, 0, "z")}, 0, true).end(),
        {{{}, {{0, "xyzw", {{0.5f, -1.5f, 1, 0.3f}}}}}}});
    v.push_back({"ps_1_1: tex / texcoord / cnd (> 0.5)",
        Asm(false, 1, 1).op(TEXLD, {D(TEX, 0)}).op(TEXCOORD, {D(TEX, 1)}).op(MOV, {D(TEMP, 0, "w"), S(INPUT, 0)})
            .op(CND, {D(TEMP, 0), S(TEMP, 0, "w"), S(TEX, 0), S(TEX, 1)}).end(),
        {{{{1, {{0, 0, 0, 0.6f}}}, {4, {{0.5f, 2, -1, 7}}}}, {{0, "xyzw", {{0.1f, 0.2f, 0.3f, 0.4f}}}}},
         {{{1, {{0, 0, 0, 0.5f}}}, {4, {{0.5f, 2, -1, 7}}}}, {{0, "xyzw", {{0.5f, 1, 0, 1}}}}}},
        tex_setup});
    return v;
}

} // namespace

int main()
{
    @autoreleasepool {
        harness::Gpu gpu;
        int failed = 0, total = 0;
        for (auto &c : cases()) {
            total++;
            std::string err;
            sm::Shader s;
            msl::Output out;
            msl::Options mo;
            mo.mode = msl::Mode::ComputeTest;
            if (!sm::decode(c.tokens.data(), c.tokens.size(), s, err) || !msl::translate(s, mo, out, err)) {
                printf("FAIL  %s\n      %s\n", c.name, err.c_str());
                failed++;
                continue;
            }
            std::vector<ref::Vec4> consts(256);
            ref::Env env;
            env.c = consts.data();
            if (c.setup) c.setup(env, consts.data());
            unsigned n = unsigned(c.invs.size());
            std::vector<ref::Vec4> in(n * TF2MT_TEST_STRIDE), g(n * TF2MT_TEST_STRIDE), r(n * TF2MT_TEST_STRIDE);
            for (unsigned i = 0; i < n; i++)
                for (auto &[slot, val] : c.invs[i].in) in[i * TF2MT_TEST_STRIDE + slot] = val;
            if (!gpu.run(out, env, in.data(), n, g.data(), err)) { printf("FAIL  %s\n      %s\n%s\n", c.name, err.c_str(), out.source.c_str()); failed++; continue; }
            std::string msg;
            for (unsigned i = 0; i < n; i++) {
                if (!ref::run(s, env, &in[i * TF2MT_TEST_STRIDE], &r[i * TF2MT_TEST_STRIDE], err)) { msg += "  cpu: " + err; break; }
                for (auto &chk : c.invs[i].expect)
                    for (const char *m = chk.m; *m; m++) {
                        int k = int(strchr("xyzw", *m) - "xyzw");
                        float e = chk.v.v[k], gv = g[i * TF2MT_TEST_STRIDE + chk.slot].v[k], rv = r[i * TF2MT_TEST_STRIDE + chk.slot].v[k];
                        char b[200];
                        if (!close(gv, e)) { snprintf(b, sizeof b, "  inv %u slot %u.%c: gpu %.9g expected %.9g\n", i, chk.slot, *m, gv, e); msg += b; }
                        if (!close(rv, e)) { snprintf(b, sizeof b, "  inv %u slot %u.%c: cpu %.9g expected %.9g\n", i, chk.slot, *m, rv, e); msg += b; }
                    }
            }
            if (msg.empty()) printf("ok    %s\n", c.name);
            else { printf("FAIL  %s\n%s", c.name, msg.c_str()); failed++; }
        }
        printf("%d/%d golden opcode cases passed\n", total - failed, total);
        return failed ? 1 : 0;
    }
}
