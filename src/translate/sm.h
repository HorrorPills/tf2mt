// tf2mt shader translator — Direct3D 9 shader bytecode model (SM1.x/2.x/3.0 token format).
// Written from Microsoft's public D3D9 documentation and the public d3d9types.h token definitions.
// Scope (ADR-001, docs/census-summary.md): vs_2_0, ps_2_0, ps_2_x, plus the engine's ps_1_1; anything else decodes
// but is rejected by the emitter/interpreter with a precise error.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tf2mt::sm {

enum class Stage : uint8_t { Vertex, Pixel };

enum RegType : uint8_t {
    TEMP = 0, INPUT = 1, CONST = 2, ADDR = 3 /* vs: a0 | ps: t# (TEXTURE) */, RASTOUT = 4, ATTROUT = 5,
    TEXCRDOUT = 6 /* OUTPUT in vs_3_0 */, CONSTINT = 7, COLOROUT = 8, DEPTHOUT = 9, SAMPLER = 10,
    CONST2 = 11, CONST3 = 12, CONST4 = 13, CONSTBOOL = 14, LOOP = 15, TEMPFLOAT16 = 16, MISCTYPE = 17,
    LABEL = 18, PREDICATE = 19,
};

enum Op : uint16_t {
    NOP = 0, MOV = 1, ADD = 2, SUB = 3, MAD = 4, MUL = 5, RCP = 6, RSQ = 7, DP3 = 8, DP4 = 9, MIN = 10, MAX = 11,
    SLT = 12, SGE = 13, EXP = 14, LOG = 15, LIT = 16, DST = 17, LRP = 18, FRC = 19, M4x4 = 20, M4x3 = 21, M3x4 = 22,
    M3x3 = 23, M3x2 = 24, CALL = 25, CALLNZ = 26, LOOP_ = 27, RET = 28, ENDLOOP = 29, LABEL_ = 30, DCL = 31, POW = 32,
    CRS = 33, SGN = 34, ABS = 35, NRM = 36, SINCOS = 37, REP = 38, ENDREP = 39, IF = 40, IFC = 41, ELSE = 42,
    ENDIF = 43, BREAK = 44, BREAKC = 45, MOVA = 46, DEFB = 47, DEFI = 48, TEXCOORD = 64, TEXKILL = 65, TEX = 66,
    TEXBEM = 67, EXPP = 78, LOGP = 79, CND = 80, DEF = 81, TEXDEPTH = 87, CMP = 88, BEM = 89, DP2ADD = 90, DSX = 91,
    DSY = 92, TEXLDD = 93, SETP = 94, TEXLDL = 95, BREAKP = 96, PHASE = 0xfffd, COMMENT = 0xfffe, END = 0xffff,
};

enum SrcMod : uint8_t { SM_NONE = 0, SM_NEG = 1, SM_BIAS = 2, SM_BIASNEG = 3, SM_SIGN = 4, SM_SIGNNEG = 5, SM_COMP = 6,
                        SM_X2 = 7, SM_X2NEG = 8, SM_DZ = 9, SM_DW = 10, SM_ABS = 11, SM_ABSNEG = 12, SM_NOT = 13 };

// Texture types from dcl on sampler registers (D3DSTT_* >> 27)
enum TexType : uint8_t { TT_UNKNOWN = 0, TT_1D = 1, TT_2D = 2, TT_CUBE = 3, TT_VOLUME = 4 };

// Comparison (ifc/breakc/setp control field)
enum Cmp : uint8_t { CMP_GT = 1, CMP_EQ = 2, CMP_GE = 3, CMP_LT = 4, CMP_NE = 5, CMP_LE = 6 };

struct Src {
    RegType type = TEMP;
    uint32_t index = 0;
    uint8_t swz[4] = {0, 1, 2, 3};      // component selected for x, y, z, w
    SrcMod mod = SM_NONE;
    bool relative = false;              // index += relative register component
    RegType rel_type = ADDR;            // a0 (vs) or aL (LOOP)
    uint32_t rel_index = 0;
    uint8_t rel_comp = 0;               // component of the relative register
};

struct Dst {
    RegType type = TEMP;
    uint32_t index = 0;
    uint8_t mask = 0xf;                 // bit 0 = x ... bit 3 = w
    bool saturate = false, partial = false, centroid = false;
    int8_t shift = 0;                   // ps_1_x result shift (SM2+: must be 0)
};

struct Instr {
    uint16_t op = NOP;
    uint8_t control = 0;                // bits 16..23 of the instruction token (texld variants, comparisons)
    bool predicated = false;
    bool coissue = false;               // ps_1_x '+' pairing
    Src pred;                           // predicate register source when predicated
    bool has_dst = false;
    Dst dst;
    uint8_t nsrc = 0;
    Src src[5];                         // 4 + transient predicate slot during decode
    // DCL
    uint32_t dcl_usage = 0, dcl_usage_index = 0;
    TexType dcl_tex = TT_UNKNOWN;
    // DEF / DEFI / DEFB literal values
    uint32_t def_bits[4] = {0, 0, 0, 0};
    uint32_t token_offset = 0;          // for error messages
};

struct Shader {
    Stage stage = Stage::Pixel;
    uint8_t major = 0, minor = 0;       // ps_2_x encodes as 2.1
    std::vector<Instr> instrs;
    uint32_t token_count = 0;
};

// Decode a complete token stream (version token .. END). Returns false with a precise message on malformed input.
bool decode(const uint32_t *tokens, size_t count, Shader &out, std::string &error);

const char *op_name(uint16_t op);
std::string version_string(const Shader &s);   // e.g. "ps_2_x", "vs_2_0"

} // namespace tf2mt::sm
