// Shared GPU runner for translator tests (ObjC++, header-only): compile a Mode::ComputeTest translation and execute
// it for N invocations with the msl_abi.h test-harness layout. Used by tools/translate/difftest.mm and
// tests/unit/opcodes.mm.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../../src/translate/interp.h"
#include "../../src/translate/msl.h"
#include "../../src/translate/msl_abi.h"
#include <cstring>
#include <string>
#include <vector>

namespace tf2mt::harness {

struct Gpu {
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    MTLCompileOptions *opts;
    id<MTLSamplerState> smp;

    Gpu()
    {
        dev = MTLCreateSystemDefaultDevice();
        queue = [dev newCommandQueue];
        opts = [MTLCompileOptions new];
        opts.mathMode = MTLMathModeRelaxed;           // keeps INF/NaN semantics (ADR-003)
        opts.languageVersion = MTLLanguageVersion3_1;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        smp = [dev newSamplerStateWithDescriptor:sd];
    }

    // consts: 256 entries; in/out: n * TF2MT_TEST_STRIDE entries. Textures: 1x1 constant colours env.tex[s#].
    bool run(const msl::Output &o, const ref::Env &env, const ref::Vec4 *in, unsigned n, ref::Vec4 *out, std::string &err)
    {
        @autoreleasepool {
            NSError *nerr = nil;
            NSString *src = [[NSString alloc] initWithBytes:o.source.data() length:o.source.size() encoding:NSUTF8StringEncoding];
            id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opts error:&nerr];
            id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"tf2mt_test"] : nil;
            id<MTLComputePipelineState> pso = fn ? [dev newComputePipelineStateWithFunction:fn error:&nerr] : nil;
            if (!pso) { err = std::string("compile: ") + (nerr ? nerr.localizedDescription.UTF8String : "?"); return false; }
            tf2mt_int_consts ic = {};
            memcpy(ic.i, env.i, sizeof ic.i);
            ic.b = env.b;
            id<MTLBuffer> bc = [dev newBufferWithBytes:env.c length:256 * 16 options:MTLResourceStorageModeShared];
            id<MTLBuffer> bi = [dev newBufferWithBytes:&ic length:sizeof ic options:MTLResourceStorageModeShared];
            id<MTLBuffer> bin = [dev newBufferWithBytes:in length:size_t(n) * TF2MT_TEST_STRIDE * 16 options:MTLResourceStorageModeShared];
            id<MTLBuffer> bout = [dev newBufferWithLength:size_t(n) * TF2MT_TEST_STRIDE * 16 options:MTLResourceStorageModeShared];
            memset(bout.contents, 0, bout.length);
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:bc offset:0 atIndex:0];
            [enc setBuffer:bi offset:0 atIndex:1];
            [enc setBuffer:bin offset:0 atIndex:10];
            [enc setBuffer:bout offset:0 atIndex:11];
            for (unsigned k = 0; k < 16; k++) {
                if (!(o.refl.sampler_mask & (1u << k))) continue;
                MTLTextureDescriptor *td = [MTLTextureDescriptor new];
                td.pixelFormat = MTLPixelFormatRGBA32Float;
                td.width = td.height = 1;
                td.textureType = o.refl.sampler_type[k] == sm::TT_CUBE ? MTLTextureTypeCube
                               : o.refl.sampler_type[k] == sm::TT_VOLUME ? MTLTextureType3D : MTLTextureType2D;
                id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
                unsigned faces = td.textureType == MTLTextureTypeCube ? 6 : 1;
                for (unsigned f = 0; f < faces; f++)
                    [tex replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:f withBytes:env.tex[k].v bytesPerRow:16 bytesPerImage:16];
                [enc setTexture:tex atIndex:k];
                [enc setSamplerState:smp atIndex:k];
            }
            [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(n < 64 ? n : 64, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) { err = std::string("gpu: ") + cb.error.localizedDescription.UTF8String; return false; }
            memcpy(out, bout.contents, bout.length);
            return true;
        }
    }
};

} // namespace tf2mt::harness
