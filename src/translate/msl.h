// tf2mt shader translator — D3D9 shader (sm.h) -> Metal Shading Language source. ABI: msl_abi.h.
// Design record: docs/decisions/ADR-003-shader-translator.md.
#pragma once
#include "sm.h"
#include <cstdint>
#include <string>

namespace tf2mt::msl {

enum class Mode {
    Production,   // vertex / fragment entry point using the msl_abi.h bindings
    ComputeTest,  // kernel "tf2mt_test": one invocation per thread, inputs/outputs in raw float4 buffers, explicit-LOD
                  // sampling, texkill recorded instead of discarding (differential testing against tools/translate)
};

struct Options {
    Mode mode = Mode::Production;
    bool legacy_mul = true;      // D3D9 0 * x = 0 in mul/mad/dp*/pow (docs/semantics.md)
};

struct Reflection {
    sm::Stage stage = sm::Stage::Pixel;
    // VS: declared inputs (v# -> D3DDECLUSAGE, usage index); the backend builds tf2mt_fetch entries from these.
    uint16_t vs_input_mask = 0;
    uint8_t vs_input_usage[16] = {}, vs_input_usage_index[16] = {};
    // Varyings: VS -> written slots, PS -> read slots (bit = tf2mt_varying)
    uint32_t varying_mask = 0;
    // Samplers: bit n = s# used; type per register (sm::TexType)
    uint16_t sampler_mask = 0;
    uint8_t sampler_type[16] = {};
    uint8_t color_out_mask = 0;  // PS oC# written
    bool writes_depth = false, uses_texkill = false, uses_int_consts = false, uses_relative = false;
    uint32_t float_consts_used = 0;  // highest c# read + 1 (relative addressing -> full range)
};

struct Output {
    std::string source;
    std::string entry;           // "tf2mt_vs", "tf2mt_ps" or "tf2mt_test"
    Reflection refl;
};

// Translate a decoded shader. On failure returns false and `error` names the instruction and the unsupported
// feature (PLAN D9: anything outside the census scope fails loudly).
bool translate(const sm::Shader &shader, const Options &opt, Output &out, std::string &error);

} // namespace tf2mt::msl
