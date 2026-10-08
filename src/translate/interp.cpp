// CPU reference interpreter — see interp.h. Semantics rulings: docs/semantics.md.
#include "interp.h"
#include "msl_abi.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace tf2mt::ref {
using namespace sm;

namespace {

struct V { float x[4]; };

float sat(float f) { return std::fmin(std::fmax(f, 0.0f), 1.0f); }

struct Machine {
    const Shader &s;
    const Env &e;
    const Vec4 *in;
    Vec4 *out;
    std::string &err;
    bool vs = false, sm1 = false, sm3 = false;

    V r[32] = {}, t[8] = {}, v[16] = {}, o[TF2MT_VAR_COUNT] = {}, oc[4] = {}, odepth = {};
    int a0[4] = {};
    bool killed = false;
    uint8_t oc_written = 0;
    bool depth_written = false;
    bool hasdef[256] = {}, hasdefi[16] = {}, hasdefb[16] = {};
    V def[256] = {};
    int defi[16][4] = {};
    bool defb[16] = {};
    int out_slot[16], in_slot[16];
    const Instr *cur = nullptr;

    Machine(const Shader &sh, const Env &en, const Vec4 *i, Vec4 *ou, std::string &er)
        : s(sh), e(en), in(i), out(ou), err(er)
    {
        for (int &x : out_slot) x = -1;
        for (int &x : in_slot) x = -1;
        for (auto &x : o) x = V{{0, 0, 0, 1}};
    }

    bool fail(const char *what)
    {
        char b[256];
        snprintf(b, sizeof b, "interp: token %u (%s): %s", cur ? cur->token_offset : 0, cur ? op_name(cur->op) : "-", what);
        err = b;
        return false;
    }

    float mul(float a, float b) const { return e.legacy_mul && (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }

    static int usage_slot(uint32_t usage, uint32_t idx)
    {
        if (usage == 0 && idx == 0) return TF2MT_VAR_POS;
        if (usage == 4 && idx == 0) return TF2MT_VAR_PSIZE;
        if (usage == 5 && idx < 8) return TF2MT_VAR_T0 + int(idx);
        if (usage == 10 && idx < 2) return TF2MT_VAR_C0 + int(idx);
        if (usage == 11 && idx == 0) return TF2MT_VAR_FOG;
        return -1;
    }

    V constant(int idx)
    {
        V c;
        if (idx >= 0 && idx < 256 && hasdef[idx]) c = def[idx];
        else if (idx >= 0 && idx < (vs ? TF2MT_VS_FLOAT_CONSTS : TF2MT_PS_FLOAT_CONSTS)) memcpy(c.x, e.c[idx].v, 16);
        else c = V{{0, 0, 0, 0}};
        if (sm1 && !vs) for (float &f : c.x) f = std::fmin(std::fmax(f, -1.0f), 1.0f);
        return c;
    }

    V input_slot(int slot) { V x; memcpy(x.x, in[slot].v, 16); return x; }

    bool base(const Src &sr, V &b)
    {
        switch (sr.type) {
        case TEMP: if (sr.index >= 32) return fail("temp index"); b = r[sr.index]; return true;
        case INPUT:
            if (vs) { if (sr.index >= 16) return fail("input index"); b = v[sr.index]; return true; }
            if (sm3) { if (sr.index >= 16 || in_slot[sr.index] < 0) return fail("undeclared input"); b = input_slot(in_slot[sr.index]); return true; }
            if (sr.index >= 2) return fail("colour input index");
            b = input_slot(TF2MT_VAR_C0 + int(sr.index));
            return true;
        case CONST: {
            int idx = int(sr.index);
            if (sr.relative) idx += a0[sr.rel_comp];
            b = constant(idx);
            return true;
        }
        case ADDR:
            if (vs) { for (int k = 0; k < 4; k++) b.x[k] = float(a0[k]); return true; }
            if (sr.index >= 8) return fail("texture register index");
            b = sm1 ? t[sr.index] : input_slot(TF2MT_VAR_T0 + int(sr.index));
            return true;
        default: return fail("source register type");
        }
    }

    static float modify(SrcMod m, float x, bool &ok)
    {
        ok = true;
        switch (m) {
        case SM_NONE: return x;
        case SM_NEG: return -x;
        case SM_BIAS: return x - 0.5f;
        case SM_BIASNEG: return 0.5f - x;
        case SM_SIGN: return 2.0f * x - 1.0f;
        case SM_SIGNNEG: return 1.0f - 2.0f * x;
        case SM_COMP: return 1.0f - x;
        case SM_X2: return 2.0f * x;
        case SM_X2NEG: return -2.0f * x;
        case SM_ABS: return std::fabs(x);
        case SM_ABSNEG: return -std::fabs(x);
        default: ok = false; return x;
        }
    }

    bool read(int i, V &res)
    {
        if (i >= cur->nsrc) return fail("missing operand");
        const Src &sr = cur->src[i];
        V b;
        if (!base(sr, b)) return false;
        for (int k = 0; k < 4; k++) {
            bool ok;
            res.x[k] = modify(sr.mod, b.x[sr.swz[k]], ok);
            if (!ok) return fail("source modifier");
        }
        return true;
    }

    bool scalar(int i, float &f)
    {
        V x;
        if (!read(i, x)) return false;
        f = x.x[3];
        return true;
    }

    bool write(const Dst &d, V val)
    {
        for (float &f : val.x) {
            if (d.shift) f = f * std::ldexp(1.0f, d.shift);
            if (d.saturate) f = sat(f);
        }
        V *target = nullptr;
        switch (d.type) {
        case TEMP: if (d.index >= 32) return fail("temp index"); target = &r[d.index]; break;
        case ADDR:
            if (vs) { for (int k = 0; k < 4; k++) if (d.mask & (1 << k)) a0[k] = int(std::floor(val.x[k] + 0.5f)); return true; }
            if (!sm1 || d.index >= 8) return fail("texture register write");
            target = &t[d.index];
            break;
        case RASTOUT:
            if (d.index > 2) return fail("rastout index");
            target = &o[d.index == 0 ? TF2MT_VAR_POS : d.index == 1 ? TF2MT_VAR_FOG : TF2MT_VAR_PSIZE];
            break;
        case ATTROUT: if (d.index > 1) return fail("oD index"); target = &o[TF2MT_VAR_C0 + d.index]; break;
        case TEXCRDOUT:
            if (sm3) { if (d.index >= 16 || out_slot[d.index] < 0) return fail("undeclared output"); target = &o[out_slot[d.index]]; }
            else { if (d.index >= 8) return fail("oT index"); target = &o[TF2MT_VAR_T0 + d.index]; }
            break;
        case COLOROUT: if (d.index > 3) return fail("oC index"); target = &oc[d.index]; oc_written |= 1u << d.index; break;
        case DEPTHOUT: target = &odepth; depth_written = true; break;
        default: return fail("destination register type");
        }
        for (int k = 0; k < 4; k++) if (d.mask & (1 << k)) target->x[k] = val.x[k];
        return true;
    }

    V splat(float f) { return V{{f, f, f, f}}; }

    // Value of an arithmetic instruction (before destination modifiers / mask).
    bool compute(V &res)
    {
        const Instr &in = *cur;
        V a, b, c;
        float x, y;
        switch (in.op) {
        case MOV: case MOVA: return read(0, res);
        case ADD: if (!read(0, a) || !read(1, b)) return false; for (int k = 0; k < 4; k++) res.x[k] = a.x[k] + b.x[k]; return true;
        case MUL: if (!read(0, a) || !read(1, b)) return false; for (int k = 0; k < 4; k++) res.x[k] = mul(a.x[k], b.x[k]); return true;
        case MAD:
            if (!read(0, a) || !read(1, b) || !read(2, c)) return false;
            for (int k = 0; k < 4; k++) res.x[k] = mul(a.x[k], b.x[k]) + c.x[k];
            return true;
        case DP3: if (!read(0, a) || !read(1, b)) return false; res = splat(mul(a.x[0], b.x[0]) + mul(a.x[1], b.x[1]) + mul(a.x[2], b.x[2])); return true;
        case DP4:
            if (!read(0, a) || !read(1, b)) return false;
            res = splat(mul(a.x[0], b.x[0]) + mul(a.x[1], b.x[1]) + mul(a.x[2], b.x[2]) + mul(a.x[3], b.x[3]));
            return true;
        case DP2ADD:
            if (!read(0, a) || !read(1, b) || !read(2, c)) return false;
            res = splat(mul(a.x[0], b.x[0]) + mul(a.x[1], b.x[1]) + c.x[0]);
            return true;
        case MIN: if (!read(0, a) || !read(1, b)) return false; for (int k = 0; k < 4; k++) res.x[k] = std::fmin(a.x[k], b.x[k]); return true;
        case MAX: if (!read(0, a) || !read(1, b)) return false; for (int k = 0; k < 4; k++) res.x[k] = std::fmax(a.x[k], b.x[k]); return true;
        case SLT: if (!read(0, a) || !read(1, b)) return false; for (int k = 0; k < 4; k++) res.x[k] = a.x[k] < b.x[k] ? 1.0f : 0.0f; return true;
        case SGE: if (!read(0, a) || !read(1, b)) return false; for (int k = 0; k < 4; k++) res.x[k] = a.x[k] >= b.x[k] ? 1.0f : 0.0f; return true;
        case ABS: if (!read(0, a)) return false; for (int k = 0; k < 4; k++) res.x[k] = std::fabs(a.x[k]); return true;
        case FRC:
            if (!read(0, a)) return false;
            for (int k = 0; k < 4; k++) res.x[k] = std::fmin(a.x[k] - std::floor(a.x[k]), 0x1.fffffep-1f);
            return true;
        case NRM: {
            if (!read(0, a)) return false;
            float l = a.x[0] * a.x[0] + a.x[1] * a.x[1] + a.x[2] * a.x[2];
            float f = l > 0.0f ? 1.0f / std::sqrt(l) : 0.0f;
            for (int k = 0; k < 4; k++) res.x[k] = l > 0.0f ? a.x[k] * f : 0.0f;
            return true;
        }
        case LRP:
            if (!read(0, a) || !read(1, b) || !read(2, c)) return false;
            for (int k = 0; k < 4; k++) res.x[k] = c.x[k] + a.x[k] * (b.x[k] - c.x[k]);
            return true;
        case CMP:
            if (!read(0, a) || !read(1, b) || !read(2, c)) return false;
            for (int k = 0; k < 4; k++) res.x[k] = a.x[k] >= 0.0f ? b.x[k] : c.x[k];
            return true;
        case CND:
            if (!read(0, a) || !read(1, b) || !read(2, c)) return false;
            for (int k = 0; k < 4; k++) res.x[k] = a.x[k] > 0.5f ? b.x[k] : c.x[k];
            return true;
        case RCP: if (!scalar(0, x)) return false; res = splat(x == 0.0f ? INFINITY : 1.0f / x); return true;
        case RSQ: if (!scalar(0, x)) return false; res = splat(1.0f / std::sqrt(std::fabs(x))); return true;
        case EXP: if (!scalar(0, x)) return false; res = splat(std::exp2(x)); return true;
        case LOG: if (!scalar(0, x)) return false; res = splat(std::log2(std::fabs(x))); return true;
        case POW: if (!scalar(0, x) || !scalar(1, y)) return false; res = splat(std::exp2(mul(y, std::log2(std::fabs(x))))); return true;
        case SINCOS: if (!scalar(0, x)) return false; res = V{{std::cos(x), std::sin(x), 0.0f, 0.0f}}; return true;
        default: return fail("opcode not interpreted");
        }
    }

    bool texture(V &res)
    {
        const Instr &in = *cur;
        if (sm1) { memcpy(res.x, e.tex[in.dst.index].v, 16); return true; }
        if (in.nsrc < 2 || in.src[1].type != SAMPLER || in.src[1].index >= 16) return fail("texld sampler operand");
        V coord;
        if (!read(0, coord)) return false;   // coordinates do not matter for constant test textures, but validate
        const Src &sr = in.src[1];
        const float *tx = e.tex[sr.index].v;
        for (int k = 0; k < 4; k++) res.x[k] = tx[sr.swz[k]];
        return true;
    }

    bool exec()
    {
        vs = s.stage == Stage::Vertex;
        sm1 = s.major == 1 && !vs;
        sm3 = s.major == 3;
        // definitions and declarations
        for (auto &in : s.instrs) {
            cur = &in;
            if (in.op == DEF && in.dst.index < 256) { hasdef[in.dst.index] = true; memcpy(def[in.dst.index].x, in.def_bits, 16); }
            if (in.op == DEFI && in.dst.index < 16) { hasdefi[in.dst.index] = true; for (int k = 0; k < 4; k++) defi[in.dst.index][k] = int32_t(in.def_bits[k]); }
            if (in.op == DEFB && in.dst.index < 16) { hasdefb[in.dst.index] = true; defb[in.dst.index] = in.def_bits[0] != 0; }
            if (in.op == DCL && sm3 && in.dst.index < 16) {
                if (vs && in.dst.type == TEXCRDOUT) out_slot[in.dst.index] = usage_slot(in.dcl_usage, in.dcl_usage_index);
                if (!vs && in.dst.type == INPUT) in_slot[in.dst.index] = usage_slot(in.dcl_usage, in.dcl_usage_index);
            }
        }
        if (vs) for (int k = 0; k < 16; k++) memcpy(v[k].x, in[k].v, 16);
        // structured control flow: match rep/endrep and if/else/endif
        size_t n = s.instrs.size();
        std::vector<size_t> match(n, 0), stack;
        for (size_t i = 0; i < n; i++) {
            uint16_t op = s.instrs[i].op;
            if (op == REP || op == IF) stack.push_back(i);
            else if (op == ELSE) { if (stack.empty()) return fail("else without if"); match[stack.back()] = i; stack.back() = i; }
            else if (op == ENDREP || op == ENDIF) { if (stack.empty()) return fail("unbalanced flow"); match[stack.back()] = i; match[i] = stack.back(); stack.pop_back(); }
        }
        struct Loop { size_t start; int left; };
        std::vector<Loop> loops;
        for (size_t pc = 0; pc < n; pc++) {
            const Instr &in = s.instrs[pc];
            cur = &in;
            if (in.predicated) return fail("predication");
            switch (in.op) {
            case NOP: case DCL: case DEF: case DEFI: case DEFB: case ENDIF: continue;
            case REP: {
                const Src &sr = in.src[0];
                if (sr.type != CONSTINT || sr.index >= 16) return fail("rep operand");
                int cnt = hasdefi[sr.index] ? defi[sr.index][0] : e.i[sr.index][0];
                cnt = cnt < 0 ? 0 : cnt > 255 ? 255 : cnt;
                if (cnt == 0) pc = match[pc];
                else loops.push_back({pc, cnt});
                continue;
            }
            case ENDREP:
                if (loops.empty()) return fail("endrep");
                if (--loops.back().left > 0) pc = loops.back().start;
                else loops.pop_back();
                continue;
            case IF: {
                const Src &sr = in.src[0];
                if (sr.type != CONSTBOOL || sr.index >= 16) return fail("if operand");
                bool cond = hasdefb[sr.index] ? defb[sr.index] : ((e.b >> sr.index) & 1u) != 0;
                if (!cond) pc = match[pc];      // to ELSE (then continue after it) or ENDIF
                continue;
            }
            case ELSE: {
                // reached by falling out of the taken branch: skip to the matching ENDIF
                size_t j = pc;
                while (s.instrs[j].op != ENDIF) j = match[j];
                pc = j;
                continue;
            }
            case TEXKILL: {
                const Dst &d = in.dst;
                V x;
                if (d.type == TEMP && d.index < 32) x = r[d.index];
                else if (d.type == ADDR && d.index < 8) x = input_slot(TF2MT_VAR_T0 + int(d.index));
                else return fail("texkill operand");
                uint8_t m = sm1 ? 0x7 : d.mask;
                for (int k = 0; k < 4; k++) if ((m & (1 << k)) && x.x[k] < 0.0f) killed = true;
                continue;
            }
            case TEXCOORD: {
                V x = input_slot(TF2MT_VAR_T0 + int(in.dst.index));
                V val{{sat(x.x[0]), sat(x.x[1]), sat(x.x[2]), 1.0f}};
                if (!write(in.dst, val)) return false;
                continue;
            }
            case TEX: case TEXLDL: {
                V val;
                if (!texture(val) || !write(in.dst, val)) return false;
                continue;
            }
            default: break;
            }
            if (pc + 1 < n && s.instrs[pc + 1].coissue) {
                V a, b;
                if (!compute(a)) return false;
                cur = &s.instrs[pc + 1];
                if (!compute(b)) return false;
                if (!write(in.dst, a) || !write(s.instrs[pc + 1].dst, b)) return false;
                pc++;
                continue;
            }
            V val;
            if (!compute(val) || !write(in.dst, val)) return false;
        }
        cur = nullptr;
        // outputs in test-harness layout
        if (vs) {
            for (int slot = 0; slot < TF2MT_VAR_COUNT; slot++) {
                V x = o[slot];
                if (!sm3 && (slot == TF2MT_VAR_C0 || slot == TF2MT_VAR_C1)) for (float &f : x.x) f = sat(f);
                memcpy(out[slot].v, x.x, 16);
            }
        } else {
            if (sm1) { oc[0] = r[0]; oc_written |= 1; }
            for (int k = 0; k < 4; k++) if (oc_written & (1 << k)) memcpy(out[TF2MT_PSO_C0 + k].v, oc[k].x, 16);
            if (depth_written) memcpy(out[TF2MT_PSO_DEPTH].v, odepth.x, 16);
            float kf = killed ? 1.0f : 0.0f;
            out[TF2MT_PSO_KILL] = Vec4{{kf, kf, kf, kf}};
        }
        return true;
    }
};

} // namespace

bool run(const Shader &s, const Env &env, const Vec4 *in, Vec4 *out, std::string &error)
{
    error.clear();
    Machine m(s, env, in, out, error);
    return m.exec();
}

} // namespace tf2mt::ref
