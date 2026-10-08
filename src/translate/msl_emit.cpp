// D3D9 shader -> MSL emitter. One pass over the decoded instruction list; the Metal compiler does SSA, register
// allocation and dead-lane elimination (ADR-003), so every D3D register becomes a float4 local and every
// instruction a masked float4 assignment. Semantics rulings: docs/semantics.md.
#include "msl.h"
#include "msl_abi.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace tf2mt::msl {
using namespace sm;

namespace {

const char *kPrelude =
#include "msl_prelude.inc"
    ;

const char C4[] = "xyzw";

const char *slot_name(int slot)
{
    static const char *n[] = {"pos", "c0", "c1", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "fog", "psize"};
    return n[slot];
}

std::string flit(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    char b[64];
    if (!std::isfinite(f)) { snprintf(b, sizeof b, "as_type<float>(0x%08xu)", bits); return b; }
    snprintf(b, sizeof b, "%.9g", f);
    std::string s = b;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s + "f";
}

std::string flit(float f) { uint32_t u; memcpy(&u, &f, 4); return flit(u); }

std::string mask_str(uint8_t m)
{
    std::string s;
    for (int i = 0; i < 4; i++) if (m & (1 << i)) s += C4[i];
    return s;
}

struct Emitter {
    const Shader &s;
    const Options &o;
    Output &out;
    std::string &err;
    Reflection &R;
    bool vs = false, sm1 = false, sm3 = false, test = false;
    const Instr *cur = nullptr;

    std::string body;
    int indent = 1;
    std::map<uint32_t, std::array<uint32_t, 4>> defc, defi;
    std::map<uint32_t, uint32_t> defb;
    std::set<uint32_t> temps, tex_regs;        // r#, ps_1_x t# (written by tex/texcoord)
    int out_slot[16];                          // vs_3_0: o# -> varying slot
    int in_slot[16];                           // ps_3_0: v# -> varying slot
    bool centroid[TF2MT_VAR_COUNT] = {};
    bool vs_input_decl[16] = {};
    bool uses_a0 = false, uses_crel = false, uses_lod_bias = false;
    uint32_t vs_written = 0;                   // varying slots written by the VS
    int rep_depth = 0;

    Emitter(const Shader &sh, const Options &op, Output &ou, std::string &e)
        : s(sh), o(op), out(ou), err(e), R(ou.refl)
    {
        for (int &x : out_slot) x = -1;
        for (int &x : in_slot) x = -1;
    }

    bool fail(const char *fmt, const char *a = "", unsigned b = 0)
    {
        char m[256], msg[256];
        snprintf(m, sizeof m, fmt, a, b);
        if (cur) snprintf(msg, sizeof msg, "%s token %u (%s): %s", version_string(s).c_str(), cur->token_offset, op_name(cur->op), m);
        else snprintf(msg, sizeof msg, "%s: %s", version_string(s).c_str(), m);
        err = msg;
        return false;
    }

    void line(const std::string &l) { body.append(size_t(indent) * 4, ' '); body += l; body += '\n'; }

    // ---------------------------------------------------------------- pre-pass: declarations and definitions
    bool declare()
    {
        for (auto &in : s.instrs) {
            cur = &in;
            if (in.op == DEF) defc[in.dst.index] = {in.def_bits[0], in.def_bits[1], in.def_bits[2], in.def_bits[3]};
            else if (in.op == DEFI) defi[in.dst.index] = {in.def_bits[0], in.def_bits[1], in.def_bits[2], in.def_bits[3]};
            else if (in.op == DEFB) defb[in.dst.index] = in.def_bits[0];
            if (in.op != DCL) continue;
            const Dst &d = in.dst;
            if (d.type == SAMPLER) {
                if (in.dcl_tex != TT_2D && in.dcl_tex != TT_CUBE && in.dcl_tex != TT_VOLUME)
                    return fail("sampler s%s%u: unsupported texture type", "", d.index);
                R.sampler_type[d.index] = in.dcl_tex;
                continue;
            }
            if (vs && d.type == INPUT) {
                if (d.index >= 16) return fail("input v%s%u out of range", "", d.index);
                vs_input_decl[d.index] = true;
                R.vs_input_mask |= 1u << d.index;
                R.vs_input_usage[d.index] = uint8_t(in.dcl_usage);
                R.vs_input_usage_index[d.index] = uint8_t(in.dcl_usage_index);
                continue;
            }
            if (vs && d.type == TEXCRDOUT && sm3) {
                int slot = usage_slot(in.dcl_usage, in.dcl_usage_index);
                if (slot < 0) return fail("vs_3_0 output usage %s%u not mapped to a varying", "", in.dcl_usage);
                out_slot[d.index] = slot;
                continue;
            }
            if (!vs && d.type == INPUT && sm3) {
                int slot = usage_slot(in.dcl_usage, in.dcl_usage_index);
                if (slot < 0 || slot == TF2MT_VAR_POS) return fail("ps_3_0 input usage %s%u not mapped to a varying", "", in.dcl_usage);
                in_slot[d.index] = slot;
                centroid[slot] |= d.centroid;
                continue;
            }
            if (!vs && d.type == INPUT) { if (d.index < 2) centroid[TF2MT_VAR_C0 + d.index] |= d.centroid; continue; }
            if (!vs && d.type == ADDR) { if (d.index < 8) centroid[TF2MT_VAR_T0 + d.index] |= d.centroid; continue; }
            return fail("dcl of register type %s%u", "", d.type);
        }
        cur = nullptr;
        return true;
    }

    static int usage_slot(uint32_t usage, uint32_t idx)
    {
        switch (usage) {
        case 0: return idx == 0 ? TF2MT_VAR_POS : -1;          // POSITION
        case 4: return idx == 0 ? TF2MT_VAR_PSIZE : -1;        // PSIZE
        case 5: return idx < 8 ? int(TF2MT_VAR_T0 + idx) : -1;  // TEXCOORD
        case 10: return idx < 2 ? int(TF2MT_VAR_C0 + idx) : -1; // COLOR
        case 11: return idx == 0 ? TF2MT_VAR_FOG : -1;         // FOG
        default: return -1;
        }
    }

    // ---------------------------------------------------------------- operands
    bool const_name(uint32_t idx, std::string &n)
    {
        uint32_t limit = vs ? TF2MT_VS_FLOAT_CONSTS : TF2MT_PS_FLOAT_CONSTS;
        if (defc.count(idx)) { n = "k" + std::to_string(idx); return true; }
        if (idx >= limit) return fail("constant c%s%u out of range", "", idx);
        R.float_consts_used = std::max(R.float_consts_used, idx + 1);
        n = "c[" + std::to_string(idx) + "]";
        if (sm1 && !vs) n = "clamp(" + n + ", -1.0f, 1.0f)";      // ps_1_x constants are clamped to [-1, 1]
        return true;
    }

    // Base register expression (float4), before swizzle and modifier.
    bool reg_name(const Src &r, std::string &n)
    {
        char b[64];
        switch (r.type) {
        case TEMP:
            temps.insert(r.index);
            snprintf(b, sizeof b, "r%u", r.index); n = b; return true;
        case INPUT:
            if (vs) {
                if (r.index >= 16 || !vs_input_decl[r.index]) return fail("read of undeclared input v%s%u", "", r.index);
            } else if (sm3) {
                if (r.index >= 16 || in_slot[r.index] < 0) return fail("read of undeclared input v%s%u", "", r.index);
                R.varying_mask |= 1u << in_slot[r.index];
            } else {
                if (r.index >= 2) return fail("colour input v%s%u out of range", "", r.index);
                R.varying_mask |= 1u << (TF2MT_VAR_C0 + r.index);
            }
            snprintf(b, sizeof b, "v%u", r.index); n = b; return true;
        case CONST:
            if (r.relative) {
                if (!vs) return fail("relative constant addressing in a pixel shader");
                if (r.rel_type != ADDR) return fail("relative addressing via register type %s%u", "", r.rel_type);
                uses_a0 = uses_crel = R.uses_relative = true;
                R.float_consts_used = TF2MT_VS_FLOAT_CONSTS;
                snprintf(b, sizeof b, "tf2mt_crel(c, a0.%c + %u)", C4[r.rel_comp], r.index); n = b; return true;
            }
            return const_name(r.index, n);
        case ADDR:
            if (vs) { uses_a0 = true; n = "float4(a0)"; return true; }
            if (r.index >= 8) return fail("texture register t%s%u out of range", "", r.index);
            if (sm1) {
                if (!tex_regs.count(r.index)) return fail("read of t%s%u before tex/texcoord", "", r.index);
            } else {
                R.varying_mask |= 1u << (TF2MT_VAR_T0 + r.index);
            }
            snprintf(b, sizeof b, "t%u", r.index); n = b; return true;
        default:
            return fail("source register type %s%u", "", r.type);
        }
    }

    static std::string apply_mod(SrcMod m, const std::string &x, bool &ok)
    {
        ok = true;
        switch (m) {
        case SM_NONE: return x;
        case SM_NEG: return "(-" + x + ")";
        case SM_BIAS: return "(" + x + " - 0.5f)";
        case SM_BIASNEG: return "(0.5f - " + x + ")";
        case SM_SIGN: return "(2.0f * " + x + " - 1.0f)";
        case SM_SIGNNEG: return "(1.0f - 2.0f * " + x + ")";
        case SM_COMP: return "(1.0f - " + x + ")";
        case SM_X2: return "(2.0f * " + x + ")";
        case SM_X2NEG: return "(-2.0f * " + x + ")";
        case SM_ABS: return "abs(" + x + ")";
        case SM_ABSNEG: return "(-abs(" + x + "))";
        default: ok = false; return x;
        }
    }

    bool src(const Src &r, std::string &e)
    {
        std::string n;
        if (!reg_name(r, n)) return false;
        if (!(r.swz[0] == 0 && r.swz[1] == 1 && r.swz[2] == 2 && r.swz[3] == 3)) {
            n += '.';
            for (int i = 0; i < 4; i++) n += C4[r.swz[i]];
        }
        bool ok;
        e = apply_mod(r.mod, n, ok);
        if (!ok) return fail("source modifier %s%u", "", r.mod);
        return true;
    }

    // Scalar operand of rcp/rsq/exp/log/pow/sincos: the component selected by the last swizzle slot (the assembler
    // always emits a replicate swizzle; for non-replicate swizzles D3D9 hardware uses the .w slot).
    bool src_scalar(const Src &r, std::string &e)
    {
        std::string n;
        if (!reg_name(r, n)) return false;
        n += '.';
        n += C4[r.swz[3]];
        bool ok;
        e = apply_mod(r.mod, n, ok);
        if (!ok) return fail("source modifier %s%u", "", r.mod);
        return true;
    }

    // ---------------------------------------------------------------- destinations
    bool dst_name(const Dst &d, std::string &n)
    {
        char b[64];
        switch (d.type) {
        case TEMP: temps.insert(d.index); snprintf(b, sizeof b, "r%u", d.index); n = b; return true;
        case ADDR:
            if (vs) { uses_a0 = true; n = "a0"; return true; }
            if (!sm1 || d.index >= 4) return fail("write to texture register t%s%u", "", d.index);
            tex_regs.insert(d.index);
            snprintf(b, sizeof b, "t%u", d.index); n = b; return true;
        case RASTOUT:
            if (!vs || d.index > 2) return fail("rasterizer output %s%u", "", d.index);
            return vs_out(d.index == 0 ? TF2MT_VAR_POS : d.index == 1 ? TF2MT_VAR_FOG : TF2MT_VAR_PSIZE, n);
        case ATTROUT:
            if (!vs || d.index > 1) return fail("colour output oD%s%u", "", d.index);
            return vs_out(TF2MT_VAR_C0 + int(d.index), n);
        case TEXCRDOUT:
            if (!vs) return fail("texcoord output in a pixel shader");
            if (sm3) {
                if (d.index >= 16 || out_slot[d.index] < 0) return fail("write to undeclared output o%s%u", "", d.index);
                return vs_out(out_slot[d.index], n);
            }
            if (d.index >= 8) return fail("texcoord output oT%s%u", "", d.index);
            return vs_out(TF2MT_VAR_T0 + int(d.index), n);
        case COLOROUT:
            if (vs || d.index > 3) return fail("colour output oC%s%u", "", d.index);
            R.color_out_mask |= 1u << d.index;
            snprintf(b, sizeof b, "oC%u", d.index); n = b; return true;
        case DEPTHOUT:
            if (vs) return fail("depth output in a vertex shader");
            R.writes_depth = true;
            n = "oDepth"; return true;
        default:
            return fail("destination register type %s%u", "", d.type);
        }
    }

    bool vs_out(int slot, std::string &n)
    {
        vs_written |= 1u << slot;
        n = std::string("o_") + slot_name(slot);
        return true;
    }

    bool store(const Dst &d, const std::string &val)
    {
        if (d.mask == 0) return true;
        std::string n;
        if (!dst_name(d, n)) return false;
        std::string e = val;
        if (d.shift) {
            if (!sm1) return fail("result shift outside ps_1_x");
            e = "(" + e + " * " + flit(std::ldexp(1.0f, d.shift)) + ")";
        }
        if (d.saturate) e = "saturate(" + e + ")";
        if (n == "a0") e = "int4(floor(" + e + " + 0.5f))";      // mova: round to nearest
        if (d.mask == 0xf) line(n + " = " + e + ";");
        else {
            std::string m = mask_str(d.mask);
            line(n + "." + m + " = (" + e + ")." + m + ";");
        }
        return true;
    }

    // ---------------------------------------------------------------- instructions
    // Arithmetic instructions: value expression (float4) written through the destination mask.
    bool arith(const Instr &in, std::string &e)
    {
        std::string a, b, c;
        const bool L = o.legacy_mul;
        auto S = [&](int i, std::string &x) { return i < in.nsrc ? src(in.src[i], x) : fail("missing operand"); };
        auto SC = [&](int i, std::string &x) { return i < in.nsrc ? src_scalar(in.src[i], x) : fail("missing operand"); };
        switch (in.op) {
        case MOV: if (!S(0, a)) return false; e = a; return true;
        case MOVA: if (!S(0, a)) return false; e = a; return true;
        case ADD: if (!S(0, a) || !S(1, b)) return false; e = "(" + a + " + " + b + ")"; return true;
        case MUL: if (!S(0, a) || !S(1, b)) return false; e = (L ? "lmul(" : "fmul(") + a + ", " + b + ")"; return true;
        case MAD:
            if (!S(0, a) || !S(1, b) || !S(2, c)) return false;
            e = (L ? "lmad(" : "fmad(") + a + ", " + b + ", " + c + ")"; return true;
        case DP3: if (!S(0, a) || !S(1, b)) return false; e = std::string("float4(") + (L ? "ldot3(" : "fdot3(") + a + ", " + b + "))"; return true;
        case DP4: if (!S(0, a) || !S(1, b)) return false; e = std::string("float4(") + (L ? "ldot4(" : "fdot4(") + a + ", " + b + "))"; return true;
        case DP2ADD:
            if (!S(0, a) || !S(1, b) || !S(2, c)) return false;
            e = std::string("float4(") + (L ? "ldot2(" : "fdot2(") + a + ", " + b + ") + (" + c + ").x)"; return true;
        case MIN: if (!S(0, a) || !S(1, b)) return false; e = "min(" + a + ", " + b + ")"; return true;
        case MAX: if (!S(0, a) || !S(1, b)) return false; e = "max(" + a + ", " + b + ")"; return true;
        case SLT: if (!S(0, a) || !S(1, b)) return false; e = "select(float4(0.0f), float4(1.0f), " + a + " < " + b + ")"; return true;
        case SGE: if (!S(0, a) || !S(1, b)) return false; e = "select(float4(0.0f), float4(1.0f), " + a + " >= " + b + ")"; return true;
        case ABS: if (!S(0, a)) return false; e = "abs(" + a + ")"; return true;
        case FRC: if (!S(0, a)) return false; e = "fract(" + a + ")"; return true;
        case NRM: if (!S(0, a)) return false; e = "tf2mt_nrm(" + a + ")"; return true;
        case LRP: if (!S(0, a) || !S(1, b) || !S(2, c)) return false; e = "mix(" + c + ", " + b + ", " + a + ")"; return true;
        case CMP: if (!S(0, a) || !S(1, b) || !S(2, c)) return false; e = "select(" + c + ", " + b + ", " + a + " >= 0.0f)"; return true;
        case CND:
            if (!sm1) return fail("cnd outside ps_1_x");
            if (!S(0, a) || !S(1, b) || !S(2, c)) return false;
            e = "select(" + c + ", " + b + ", " + a + " > 0.5f)"; return true;
        case RCP: if (!SC(0, a)) return false; e = "float4(tf2mt_rcp(" + a + "))"; return true;
        case RSQ: if (!SC(0, a)) return false; e = "float4(rsqrt(abs(" + a + ")))"; return true;
        case EXP: if (!SC(0, a)) return false; e = "float4(exp2(" + a + "))"; return true;
        case LOG: if (!SC(0, a)) return false; e = "float4(log2(abs(" + a + ")))"; return true;
        case POW:
            if (!SC(0, a) || !SC(1, b)) return false;
            e = L ? "float4(lpow(" + a + ", " + b + "))" : "float4(exp2(" + b + " * log2(abs(" + a + "))))"; return true;
        case SINCOS:
            if (!SC(0, a)) return false;
            e = "float4(cos(" + a + "), sin(" + a + "), 0.0f, 0.0f)"; return true;
        default:
            return fail("opcode outside the census scope (PLAN D9)");
        }
    }

    // D3DSAMP_MIPMAPLODBIAS (driver constant; production fragment shaders only)
    std::string lod_bias_value(uint32_t sn)
    {
        uses_lod_bias = true;
        return "drv.lod_bias[" + std::to_string(sn / 4) + "]." + C4[sn % 4];
    }
    std::string lod_bias(uint32_t sn) { return test || vs ? std::string() : ", bias(" + lod_bias_value(sn) + ")"; }

    bool texture_decl_type(uint32_t sn, TexType &t)
    {
        if (sn >= 16) return fail("sampler s%s%u out of range", "", sn);
        t = TexType(R.sampler_type[sn]);
        if (t == TT_UNKNOWN) return fail("sampler s%s%u used without dcl", "", sn);
        R.sampler_mask |= 1u << sn;
        return true;
    }

    bool tex(const Instr &in)
    {
        char b[160];
        if (sm1) {
            // ps_1_1: tex t# samples s# at texture coordinate # (2D; ps_1_1 has no sampler declarations).
            uint32_t n = in.dst.index;
            if (in.dst.type != ADDR || n >= 4) return fail("ps_1_x tex destination");
            R.sampler_mask |= 1u << n;
            R.sampler_type[n] = TT_2D;
            R.varying_mask |= 1u << (TF2MT_VAR_T0 + n);
            snprintf(b, sizeof b, "tex%u.sample(smp%u, tc%u.xy%s)", n, n, n, test ? ", level(0.0f)" : lod_bias(n).c_str());
            return store(in.dst, b);
        }
        if (in.nsrc < 2 || in.src[1].type != SAMPLER) return fail("texld without sampler operand");
        uint32_t sn = in.src[1].index;
        TexType t;
        if (!texture_decl_type(sn, t)) return false;
        std::string c;
        if (!src(in.src[0], c)) return false;
        std::string comps = t == TT_2D ? ".xy" : ".xyz";
        std::string coord = "(" + c + ")" + comps, extra;
        if (in.op == TEXLDL) extra = ", level((" + c + ").w)";
        else if (in.control == 1) { coord = coord + " / (" + c + ").w"; extra = vs ? "" : lod_bias(sn); }
        else if (in.control == 2) extra = ", bias((" + c + ").w" + (vs ? "" : " + " + lod_bias_value(sn)) + ")";
        else if (in.control != 0) return fail("texld control %s%u", "", in.control);
        else extra = vs ? "" : lod_bias(sn);
        if (test && in.op != TEXLDL) extra = ", level(0.0f)";
        if (vs && in.op != TEXLDL) return fail("implicit-LOD texld in a vertex shader");
        std::string e = "tex" + std::to_string(sn) + ".sample(smp" + std::to_string(sn) + ", " + coord + extra + ")";
        const Src &sr = in.src[1];
        if (!(sr.swz[0] == 0 && sr.swz[1] == 1 && sr.swz[2] == 2 && sr.swz[3] == 3)) {
            e = "(" + e + ").";
            for (int i = 0; i < 4; i++) e += C4[sr.swz[i]];
        }
        return store(in.dst, e);
    }

    bool texkill(const Instr &in)
    {
        if (vs) return fail("texkill in a vertex shader");
        std::string n;
        const Dst &d = in.dst;
        if (d.type == TEMP) { temps.insert(d.index); n = "r" + std::to_string(d.index); }
        else if (d.type == ADDR && d.index < 8) {
            // ps_1_x tests the texture coordinate set; ps_2+ t# is the interpolated coordinate itself
            R.varying_mask |= 1u << (TF2MT_VAR_T0 + d.index);
            n = (sm1 ? "tc" : "t") + std::to_string(d.index);
        } else return fail("texkill register type %s%u", "", d.type);
        uint8_t m = sm1 ? 0x7 : d.mask;
        std::string ms = mask_str(m), cond;
        cond = ms.size() == 1 ? n + "." + ms + " < 0.0f" : "any(" + n + "." + ms + " < 0.0f)";
        R.uses_texkill = true;
        line("if (" + cond + ") " + (test ? "tf2mt_killed = true;" : "discard_fragment();"));
        return true;
    }

    bool int_const(const Src &r, std::string &e)
    {
        if (r.type != CONSTINT || r.index >= 16) return fail("rep/loop count register type %s%u", "", r.type);
        if (defi.count(r.index)) {
            auto &v = defi[r.index];
            e = "int4(" + std::to_string(int32_t(v[0])) + ", " + std::to_string(int32_t(v[1])) + ", " +
                std::to_string(int32_t(v[2])) + ", " + std::to_string(int32_t(v[3])) + ")";
        } else {
            R.uses_int_consts = true;
            e = "ib.i[" + std::to_string(r.index) + "]";
        }
        return true;
    }

    bool instr(size_t &i)
    {
        const Instr &in = s.instrs[i];
        cur = &in;
        if (in.predicated) return fail("predicated instruction");
        switch (in.op) {
        case NOP: case DCL: case DEF: case DEFI: case DEFB: return true;
        case TEX: case TEXLDL: return tex(in);
        case TEXCOORD:
            if (!sm1 || in.dst.type != ADDR || in.dst.index >= 4) return fail("texcoord outside ps_1_x");
            R.varying_mask |= 1u << (TF2MT_VAR_T0 + in.dst.index);
            return store(in.dst, "saturate(float4(tc" + std::to_string(in.dst.index) + ".xyz, 1.0f))");
        case TEXKILL: return texkill(in);
        case REP: {
            std::string n;
            if (in.nsrc < 1 || !int_const(in.src[0], n)) return in.nsrc < 1 ? fail("rep without operand") : false;
            int k = rep_depth++;
            line("for (int rep" + std::to_string(k) + " = 0, repn" + std::to_string(k) + " = clamp(" + n + ".x, 0, 255); rep" +
                 std::to_string(k) + " < repn" + std::to_string(k) + "; ++rep" + std::to_string(k) + ") {");
            indent++;
            return true;
        }
        case ENDREP:
            if (rep_depth == 0) return fail("endrep without rep");
            rep_depth--; indent--; line("}"); return true;
        case IF: {
            if (in.nsrc < 1 || in.src[0].type != CONSTBOOL || in.src[0].index >= 16) return fail("if on a non-boolean operand");
            uint32_t n = in.src[0].index;
            std::string cond;
            if (defb.count(n)) cond = defb[n] ? "true" : "false";
            else { R.uses_int_consts = true; cond = "((ib.b >> " + std::to_string(n) + "u) & 1u) != 0u"; }
            line("if (" + cond + ") {");
            indent++;
            return true;
        }
        case ELSE: indent--; line("} else {"); indent++; return true;
        case ENDIF: indent--; line("}"); return true;
        default: break;
        }
        if (!in.has_dst) return fail("opcode outside the census scope (PLAN D9)");
        // ps_1_x co-issue: the paired instruction reads its sources before either result is written.
        if (i + 1 < s.instrs.size() && s.instrs[i + 1].coissue) {
            const Instr &nx = s.instrs[i + 1];
            std::string e0, e1;
            if (!arith(in, e0)) return false;
            cur = &nx;
            if (nx.predicated || !nx.has_dst || !arith(nx, e1)) return err.empty() ? fail("co-issued instruction") : false;
            line("{");
            indent++;
            line("float4 co0 = " + e0 + ";");
            line("float4 co1 = " + e1 + ";");
            if (!store(in.dst, "co0") || !store(nx.dst, "co1")) return false;
            indent--;
            line("}");
            i++;
            return true;
        }
        std::string e;
        if (!arith(in, e)) return false;
        return store(in.dst, e);
    }

    // ---------------------------------------------------------------- assembly
    std::string texture_params()
    {
        std::string p;
        for (unsigned n = 0; n < 16; n++) {
            if (!(R.sampler_mask & (1u << n))) continue;
            const char *t = R.sampler_type[n] == TT_CUBE ? "texturecube<float>" : R.sampler_type[n] == TT_VOLUME ? "texture3d<float>" : "texture2d<float>";
            char b[160];
            snprintf(b, sizeof b, ",\n    %s tex%u [[texture(%u)]], sampler smp%u [[sampler(%u)]]", t, n, n, n, n);
            p += b;
        }
        return p;
    }

    std::string locals()
    {
        std::string l;
        char b[256];
        for (auto &[idx, v] : defc) {
            snprintf(b, sizeof b, "    const float4 k%u = float4(%s, %s, %s, %s);\n", idx, flit(v[0]).c_str(),
                     flit(v[1]).c_str(), flit(v[2]).c_str(), flit(v[3]).c_str());
            if (sm1 && !vs) {   // ps_1_x def constants are clamped like c#
                snprintf(b, sizeof b, "    const float4 k%u = clamp(float4(%s, %s, %s, %s), -1.0f, 1.0f);\n", idx,
                         flit(v[0]).c_str(), flit(v[1]).c_str(), flit(v[2]).c_str(), flit(v[3]).c_str());
            }
            l += b;
        }
        for (uint32_t r : temps) { snprintf(b, sizeof b, "    float4 r%u = float4(0.0f);\n", r); l += b; }
        for (uint32_t t : tex_regs) { snprintf(b, sizeof b, "    float4 t%u = float4(0.0f);\n", t); l += b; }
        if (uses_a0) l += "    int4 a0 = int4(0);\n";
        if (vs) {
            for (int slot = 0; slot < TF2MT_VAR_COUNT; slot++) {
                snprintf(b, sizeof b, "    float4 o_%s = float4(0.0f, 0.0f, 0.0f, 1.0f);\n", slot_name(slot));
                l += b;
            }
        } else {
            for (unsigned n = 0; n < 4; n++)
                if (R.color_out_mask & (1u << n)) { snprintf(b, sizeof b, "    float4 oC%u = float4(0.0f);\n", n); l += b; }
            if (R.writes_depth) l += "    float4 oDepth = float4(0.0f);\n";
        }
        if (test && !vs) l += "    bool tf2mt_killed = false;\n";
        return l;
    }

    std::string crel_helper()
    {
        if (!uses_crel) return "";
        std::string h = "static inline float4 tf2mt_crel(constant float4 *c, int i)\n{\n";
        if (!defc.empty()) {
            h += "    switch (i) {\n";
            for (auto &[idx, v] : defc)
                h += "    case " + std::to_string(idx) + ": return float4(" + flit(v[0]) + ", " + flit(v[1]) + ", " +
                     flit(v[2]) + ", " + flit(v[3]) + ");\n";
            h += "    default: break;\n    }\n";
        }
        h += "    return uint(i) < " + std::to_string(TF2MT_VS_FLOAT_CONSTS) + "u ? c[i] : float4(0.0f);\n}\n\n";
        return h;
    }

    // Reads of the VS output colour slots are clamped to [0, 1] for SM < 3 (D3D9 colour interpolators).
    std::string vs_slot_value(int slot)
    {
        std::string v = std::string("o_") + slot_name(slot);
        if (!sm3 && (slot == TF2MT_VAR_C0 || slot == TF2MT_VAR_C1)) v = "saturate(" + v + ")";
        return v;
    }

    std::string ps_input_value(int slot)
    {
        return (test ? "I[" + std::to_string(slot) + "]" : std::string("in.") + slot_name(slot));
    }

    bool assemble()
    {
        R.stage = s.stage;
        std::string src = kPrelude;
        src += "\n// translated from " + version_string(s) + " by tf2mt\n";
        src += crel_helper();
        std::string params, pro, epi;
        bool need_c = R.float_consts_used > 0 || uses_crel;
        if (need_c) params += ",\n    constant float4 *c [[buffer(0)]]";
        if (R.uses_int_consts) params += ",\n    constant tf2mt_int_consts &ib [[buffer(1)]]";
        char b[256];
        if (vs) {
            R.varying_mask = vs_written;
            for (unsigned n = 0; n < 16; n++) {
                if (!vs_input_decl[n]) continue;
                if (test) snprintf(b, sizeof b, "    float4 v%u = I[%u];\n", n, n);
                else snprintf(b, sizeof b, "    float4 v%u = tf2mt_fetch_attr(drv, %uu, vid, vb0, vb1, vb2, vb3);\n", n, n);
                pro += b;
            }
            if (test) {
                for (int slot = 0; slot < TF2MT_VAR_COUNT; slot++) {
                    snprintf(b, sizeof b, "    O[%d] = %s;\n", slot, vs_slot_value(slot).c_str());
                    epi += b;
                }
            } else {
                src += "struct tf2mt_vs_out {\n    float4 pos [[position]];\n";
                for (int slot = TF2MT_VAR_C0; slot <= TF2MT_VAR_T0 + 7; slot++)
                    src += std::string("    float4 ") + slot_name(slot) + " [[user(" + slot_name(slot) + ")]];\n";
                src += "    float clip [[clip_distance]] [1];\n";
                if (vs_written & (1u << TF2MT_VAR_PSIZE)) src += "    float psize [[point_size]];\n";
                src += "};\n\n";
                epi += "    tf2mt_vs_out out;\n";
                epi += "    out.clip[0] = (tf2mt_clip_mask & 1u) != 0u ? dot(o_pos, drv.clip_plane0) : 1.0f;\n";
                epi += "    out.pos = o_pos;\n    out.pos.xy += drv.half_pixel.xy * o_pos.w;\n";
                for (int slot = TF2MT_VAR_C0; slot <= TF2MT_VAR_T0 + 7; slot++)
                    epi += std::string("    out.") + slot_name(slot) + " = " + vs_slot_value(slot) + ";\n";
                if (vs_written & (1u << TF2MT_VAR_PSIZE)) epi += "    out.psize = o_psize.x;\n";
                epi += "    return out;\n";
                params += ",\n    constant tf2mt_vs_driver &drv [[buffer(2)]]";
                if (R.vs_input_mask)
                    params += ",\n    device const uint *vb0 [[buffer(3)]], device const uint *vb1 [[buffer(4)]],"
                              "\n    device const uint *vb2 [[buffer(5)]], device const uint *vb3 [[buffer(6)]]";
            }
        } else {
            // inputs
            for (int slot = TF2MT_VAR_C0; slot <= TF2MT_VAR_T0 + 7; slot++) {
                if (!(R.varying_mask & (1u << slot))) continue;
                if (sm3) {
                    for (unsigned n = 0; n < 16; n++)
                        if (in_slot[n] == slot) { snprintf(b, sizeof b, "    float4 v%u = %s;\n", n, ps_input_value(slot).c_str()); pro += b; }
                } else if (slot <= TF2MT_VAR_C1) {
                    snprintf(b, sizeof b, "    float4 v%d = %s;\n", slot - TF2MT_VAR_C0, ps_input_value(slot).c_str()); pro += b;
                } else {
                    snprintf(b, sizeof b, "    float4 %s%d = %s;\n", sm1 ? "tc" : "t", slot - TF2MT_VAR_T0, ps_input_value(slot).c_str()); pro += b;
                }
            }
            if (sm1) { epi += "    oC0 = r0;\n"; temps.insert(0); R.color_out_mask |= 1; }
            if (test) {
                for (unsigned n = 0; n < 4; n++)
                    if (R.color_out_mask & (1u << n)) { snprintf(b, sizeof b, "    O[%u] = oC%u;\n", n, n); epi += b; }
                if (R.writes_depth) epi += "    O[4] = oDepth;\n";
                epi += "    O[5] = float4(tf2mt_killed ? 1.0f : 0.0f);\n";
            } else {
                if (R.varying_mask) {
                    src += "struct tf2mt_ps_in {\n";
                    for (int slot = TF2MT_VAR_C0; slot <= TF2MT_VAR_T0 + 7; slot++)
                        if (R.varying_mask & (1u << slot))
                            src += std::string("    float4 ") + slot_name(slot) + " [[user(" + slot_name(slot) + ")" +
                                   (centroid[slot] ? ", centroid_perspective" : "") + "]];\n";
                    src += "};\n";
                }
                bool any_out = R.color_out_mask || R.writes_depth;
                if (any_out) {
                    src += "struct tf2mt_ps_out {\n";
                    for (unsigned n = 0; n < 4; n++)
                        if (R.color_out_mask & (1u << n)) { snprintf(b, sizeof b, "    float4 c%u [[color(%u)]];\n", n, n); src += b; }
                    if (R.writes_depth) src += "    float depth [[depth(any)]];\n";
                    src += "};\n";
                }
                src += "\n";
                if ((R.color_out_mask & 1) || uses_lod_bias)
                    params += ",\n    constant tf2mt_ps_driver &drv [[buffer(2)]]";
                if (R.color_out_mask & 1)
                    epi += "    if (tf2mt_alpha_func != 8 && !tf2mt_alpha_pass(oC0.a, drv.alpha_ref)) discard_fragment();\n";
                if (any_out) {
                    epi += "    tf2mt_ps_out out;\n";
                    for (unsigned n = 0; n < 4; n++)
                        if (R.color_out_mask & (1u << n)) { snprintf(b, sizeof b, "    out.c%u = oC%u;\n", n, n); epi += b; }
                    if (R.writes_depth) epi += "    out.depth = oDepth.x;\n";
                    epi += "    return out;\n";
                }
            }
        }
        params += texture_params();
        std::string sig;
        if (test) {
            out.entry = "tf2mt_test";
            sig = "kernel void tf2mt_test(uint tid [[thread_position_in_grid]],\n"
                  "    device const float4 *tin [[buffer(10)]], device float4 *tout [[buffer(11)]]" + params + ")\n{\n"
                  "    device const float4 *I = tin + tid * " + std::to_string(TF2MT_TEST_STRIDE) + "u;\n"
                  "    device float4 *O = tout + tid * " + std::to_string(TF2MT_TEST_STRIDE) + "u;\n";
        } else if (vs) {
            out.entry = "tf2mt_vs";
            sig = "vertex tf2mt_vs_out tf2mt_vs(uint vid [[vertex_id]]" + params + ")\n{\n";
        } else {
            out.entry = "tf2mt_ps";
            bool any_out = R.color_out_mask || R.writes_depth;
            std::string p = R.varying_mask ? "tf2mt_ps_in in [[stage_in]]" + params : params.empty() ? "" : params.substr(6);
            sig = std::string("fragment ") + (any_out ? "tf2mt_ps_out" : "void") + " tf2mt_ps(" + p + ")\n{\n";
        }
        src += sig + pro + locals() + body + epi + "}\n";
        out.source = std::move(src);
        return true;
    }

    bool run()
    {
        vs = s.stage == Stage::Vertex;
        sm1 = s.major == 1;
        sm3 = s.major == 3;
        test = o.mode == Mode::ComputeTest;
        bool ok_ver = vs ? (s.major == 1 && s.minor == 1) || (s.major == 2 && s.minor <= 1) || (s.major == 3 && s.minor == 0)
                         : (s.major == 1 && s.minor <= 3) || (s.major == 2 && s.minor <= 1) || (s.major == 3 && s.minor == 0);
        if (!ok_ver) return fail("shader version not supported");
        if (sm1 && vs) sm1 = false;   // vs_1_1 follows the vs_2_0 rules here (no clamps, no texture registers)
        if (!declare()) return false;
        for (size_t i = 0; i < s.instrs.size(); i++)
            if (!instr(i)) return false;
        cur = nullptr;
        if (rep_depth) return fail("unterminated rep");
        if (indent != 1) return fail("unbalanced control flow");
        return assemble();
    }
};

} // namespace

bool translate(const Shader &shader, const Options &opt, Output &out, std::string &error)
{
    out = Output{};
    error.clear();
    Emitter e(shader, opt, out, error);
    return e.run();
}

} // namespace tf2mt::msl
