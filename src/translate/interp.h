// tf2mt CPU reference interpreter for decoded D3D9 shaders (PLAN §8.4: "each [semantic] has a unit test against a
// CPU reference interpreter"). Deliberately independent of the MSL emitter: it shares only the decoder, and is
// written directly from the D3D9 instruction definitions + docs/semantics.md rulings.
// Its input/output layout is the compute test harness layout (msl_abi.h, TF2MT_TEST_STRIDE slots per invocation),
// so a translated shader in Mode::ComputeTest can be compared slot for slot.
#pragma once
#include "sm.h"
#include <string>

namespace tf2mt::ref {

struct Vec4 { float v[4]; };

struct Env {
    const Vec4 *c = nullptr;       // float constants (256 entries for VS, 224 for PS)
    int32_t i[16][4] = {};         // integer constants
    uint32_t b = 0;                // boolean constants (bit n = b#)
    Vec4 tex[16] = {};             // test textures: every sampler returns one constant RGBA value
    bool legacy_mul = true;
};

// Run one invocation. `in` and `out` hold TF2MT_TEST_STRIDE slots: VS in = v0..v15, VS out = varying slots;
// PS in = varying slots, PS out = oC0..3, depth, kill flag (tf2mt_ps_test_out).
bool run(const sm::Shader &s, const Env &env, const Vec4 *in, Vec4 *out, std::string &error);

} // namespace tf2mt::ref
