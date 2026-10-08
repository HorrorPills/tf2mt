// D3D9 shader token-stream decoder. Token format (public D3D9 docs / d3d9types.h):
//   instruction: opcode bits 0-15, control bits 16-23, length bits 24-27 (SM2+), predicated bit 28, coissue bit 30
//   parameter:   register number bits 0-10, register type bits 28-30 | bits 11-12 << 3, relative bit 13,
//                dst: write mask 16-19, result modifier 20-23, shift 24-27; src: swizzle 16-23, modifier 24-27
//   comment:     opcode 0xFFFE, length in DWORDs bits 16-30; end: 0x0000FFFF
#include "sm.h"
#include <cstdio>
#include <cstring>

namespace tf2mt::sm {

static RegType reg_type(uint32_t p) { return RegType(((p >> 28) & 7) | ((p >> 8) & 0x18)); }

static Dst decode_dst(uint32_t p)
{
    Dst d;
    d.type = reg_type(p);
    d.index = p & 0x7ff;
    d.mask = (p >> 16) & 0xf;
    uint32_t rm = (p >> 20) & 0xf;
    d.saturate = rm & 1; d.partial = rm & 2; d.centroid = rm & 4;
    int s = (p >> 24) & 0xf;
    d.shift = int8_t(s >= 8 ? s - 16 : s);
    return d;
}

static Src decode_src(uint32_t p)
{
    Src s;
    s.type = reg_type(p);
    s.index = p & 0x7ff;
    uint32_t sw = (p >> 16) & 0xff;
    for (int i = 0; i < 4; i++) s.swz[i] = (sw >> (2 * i)) & 3;
    s.mod = SrcMod((p >> 24) & 0xf);
    s.relative = p & (1u << 13);
    return s;
}

// Operand layout for opcodes that have a destination (everything else: sources only).
static bool has_dst(uint16_t op)
{
    switch (op) {
    case NOP: case CALL: case CALLNZ: case LOOP_: case RET: case ENDLOOP: case LABEL_: case REP: case ENDREP:
    case IF: case IFC: case ELSE: case ENDIF: case BREAK: case BREAKC: case BREAKP: case PHASE:
        return false;
    default:
        return true;
    }
}

// SM1.x has no length field: operand counts (dst + srcs) for the instructions ps_1_x/vs_1_x can contain.
static int sm1_param_count(uint16_t op)
{
    switch (op) {
    case NOP: case RET: case ENDLOOP: case ENDREP: case ELSE: case ENDIF: case BREAK: case PHASE: return 0;
    case TEX: case TEXCOORD: case TEXKILL: case TEXDEPTH: return 1;
    case MOV: case RCP: case RSQ: case EXP: case LOG: case LIT: case FRC: case EXPP: case LOGP: case ABS: case NRM:
    case MOVA: return 2;
    case ADD: case SUB: case MUL: case DP3: case DP4: case MIN: case MAX: case SLT: case SGE: case DST:
    case M4x4: case M4x3: case M3x4: case M3x3: case M3x2: case TEXBEM: case POW: case CRS: return 3;
    case MAD: case LRP: case CND: case CMP: case BEM: case DP2ADD: return 4;
    case DEF: return 5;
    default: return -1;
    }
}

bool decode(const uint32_t *t, size_t n, Shader &out, std::string &err)
{
    char buf[256];
    if (n < 2) { err = "token stream too short"; return false; }
    uint32_t ver = t[0];
    if ((ver >> 16) == 0xffff) out.stage = Stage::Pixel;
    else if ((ver >> 16) == 0xfffe) out.stage = Stage::Vertex;
    else { snprintf(buf, sizeof buf, "bad version token 0x%08x", ver); err = buf; return false; }
    out.major = (ver >> 8) & 0xff;
    out.minor = ver & 0xff;
    out.instrs.clear();
    size_t i = 1;
    for (;;) {
        if (i >= n) { err = "missing END token"; return false; }
        uint32_t tok = t[i];
        uint16_t op = tok & 0xffff;
        if (op == END) { out.token_count = uint32_t(i + 1); return true; }
        if (op == COMMENT) { i += 1 + ((tok >> 16) & 0x7fff); continue; }
        Instr in;
        in.op = op;
        in.control = (tok >> 16) & 0xff;
        in.predicated = tok & (1u << 28);
        in.coissue = tok & (1u << 30);
        in.token_offset = uint32_t(i);
        size_t len;
        if (out.major >= 2) {
            len = (tok >> 24) & 0xf;
        } else {
            int c = (op == DCL) ? 2 : sm1_param_count(op);
            if (c < 0) { snprintf(buf, sizeof buf, "token %zu: opcode %u unsupported in SM1 stream", i, op); err = buf; return false; }
            len = size_t(c);
        }
        if (i + 1 + len > n) { snprintf(buf, sizeof buf, "token %zu: instruction overruns stream", i); err = buf; return false; }
        const uint32_t *p = t + i + 1;
        size_t k = 0;
        if (op == DCL) {
            if (len < 2) { err = "dcl: too few tokens"; return false; }
            uint32_t u = p[0];
            in.dcl_usage = u & 0x1f;
            in.dcl_usage_index = (u >> 16) & 0xf;
            in.dcl_tex = TexType((u >> 27) & 0xf);
            in.has_dst = true;
            in.dst = decode_dst(p[1]);
        } else if (op == DEF || op == DEFI) {
            if (len < 5) { err = "def: too few tokens"; return false; }
            in.has_dst = true;
            in.dst = decode_dst(p[0]);
            memcpy(in.def_bits, p + 1, 16);
        } else if (op == DEFB) {
            if (len < 2) { err = "defb: too few tokens"; return false; }
            in.has_dst = true;
            in.dst = decode_dst(p[0]);
            in.def_bits[0] = p[1];
        } else {
            if (has_dst(op)) {
                // note: texkill's single operand is a destination-format parameter (its write mask selects the
                // tested components), so has_dst() is true for it.
                if (k < len) { in.has_dst = true; in.dst = decode_dst(p[k++]); }
            }
            while (k < len) {
                if (in.nsrc == 4 + (in.predicated ? 1 : 0)) { snprintf(buf, sizeof buf, "token %zu: more than 4 sources", i); err = buf; return false; }
                Src s = decode_src(p[k++]);
                if (s.relative && out.major >= 2) {
                    // SM2+: explicit relative-address token follows (a0.x / aL)
                    if (k >= len) { snprintf(buf, sizeof buf, "token %zu: missing relative address token", i); err = buf; return false; }
                    uint32_t r = p[k++];
                    s.rel_type = reg_type(r);
                    s.rel_index = r & 0x7ff;
                    s.rel_comp = (r >> 16) & 3;          // first component of its swizzle
                } else if (s.relative) {
                    s.rel_type = ADDR; s.rel_index = 0; s.rel_comp = 0;   // SM1: implicit a0.x
                }
                in.src[in.nsrc++] = s;
            }
            if (in.predicated) {
                // The predicate source token follows the destination token; pull it out of the source list.
                for (int j = 0; j < in.nsrc; j++)
                    if (in.src[j].type == PREDICATE) {
                        in.pred = in.src[j];
                        for (int m = j; m + 1 < in.nsrc; m++) in.src[m] = in.src[m + 1];
                        in.nsrc--;
                        break;
                    }
            }
        }
        out.instrs.push_back(in);
        i += 1 + len;
    }
}

const char *op_name(uint16_t op)
{
    switch (op) {
#define N(x) case x: return #x;
    N(NOP) N(MOV) N(ADD) N(SUB) N(MAD) N(MUL) N(RCP) N(RSQ) N(DP3) N(DP4) N(MIN) N(MAX) N(SLT) N(SGE) N(EXP) N(LOG)
    N(LIT) N(DST) N(LRP) N(FRC) N(M4x4) N(M4x3) N(M3x4) N(M3x3) N(M3x2) N(CALL) N(CALLNZ) N(RET) N(ENDLOOP)
    N(DCL) N(POW) N(CRS) N(SGN) N(ABS) N(NRM) N(SINCOS) N(REP) N(ENDREP) N(IF) N(IFC) N(ELSE) N(ENDIF) N(BREAK)
    N(BREAKC) N(MOVA) N(DEFB) N(DEFI) N(TEXCOORD) N(TEXKILL) N(TEX) N(TEXBEM) N(EXPP) N(LOGP) N(CND) N(DEF)
    N(TEXDEPTH) N(CMP) N(BEM) N(DP2ADD) N(DSX) N(DSY) N(TEXLDD) N(SETP) N(TEXLDL) N(BREAKP)
#undef N
    case LOOP_: return "LOOP";
    case LABEL_: return "LABEL";
    default: return "?";
    }
}

std::string version_string(const Shader &s)
{
    char b[16];
    if (s.major == 2 && s.minor == 1) snprintf(b, sizeof b, "%s_2_x", s.stage == Stage::Pixel ? "ps" : "vs");
    else snprintf(b, sizeof b, "%s_%u_%u", s.stage == Stage::Pixel ? "ps" : "vs", s.major, s.minor);
    return b;
}

} // namespace tf2mt::sm
