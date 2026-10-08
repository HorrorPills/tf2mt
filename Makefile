# tf2mt — top-level build (plain make, see PLAN.md D7)
MINGW   ?= /opt/homebrew/bin/x86_64-w64-mingw32-gcc
D3D9_H  ?= $(shell find /opt/homebrew/Cellar/mingw-w64 -path '*x86_64-w64-mingw32/include/d3d9.h' | head -1)
PECFLAGS = -O2 -std=c17 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Ibuild/gen
PELDFLAGS = -shared -static-libgcc -Wl,--enable-stdcall-fixup -s

all: trace null tools-native

build/gen/methods.h build/gen/thunks.S build/gen/ifaces.h build/gen/stubs.S build/gen/cthunks.S: tools/trace/gen_methods.py
	python3 $< $(D3D9_H) build/gen

trace: build/trace/d3d9.dll
build/trace/d3d9.dll: tools/trace/trace.c tools/trace/census.c tools/trace/capture.c tools/trace/t9.h tools/trace/trace_common.h tools/trace/d3d9.def build/gen/thunks.S build/gen/cthunks.S build/gen/methods.h
	@mkdir -p $(@D)
	$(MINGW) $(PECFLAGS) $(PELDFLAGS) -o $@ tools/trace/trace.c tools/trace/census.c tools/trace/capture.c build/gen/thunks.S build/gen/cthunks.S tools/trace/d3d9.def

clean:
	rm -rf build

.PHONY: all trace null tools-native clean

null: build/null/d3d9.dll
build/null/d3d9.dll: tools/null/null.c build/gen/thunks.S build/gen/stubs.S build/gen/methods.h build/gen/ifaces.h
	@mkdir -p $(@D)
	$(MINGW) $(PECFLAGS) $(PELDFLAGS) -o $@ tools/null/null.c build/gen/thunks.S build/gen/stubs.S

tools-native: build/tools/threadmon
build/tools/threadmon: tools/bench/threadmon.c
	@mkdir -p $(@D)
	clang -O2 -Wall -o $@ $<
tools-native: build/tools/displaystate
build/tools/displaystate: tools/bench/displaystate.c
	@mkdir -p $(@D)
	clang -O2 -Wall -framework CoreGraphics -framework CoreFoundation -o $@ $<

tools-native: build/tools/libfnv.dylib
build/tools/libfnv.dylib: tools/census/fnv.c
	@mkdir -p $(@D)
	clang -O2 -shared -o $@ $<

# ---------------------------------------------------------------- tf2mt renderer (M2+)
UNIXCC = clang -arch x86_64 -mmacosx-version-min=13.0
frontend: build/tf2mt/tf2mt.dll build/tf2mt/d3d9.dll
unixlib: build/tf2mt/tf2mt.so

build/tf2mt/libntdll_wine.a: src/frontend/ntdll_wine.def
	@mkdir -p $(@D)
	x86_64-w64-mingw32-dlltool -d $< -l $@
build/tf2mt/tf2mt.dll: src/frontend/device.c src/frontend/adapter.c src/frontend/caps_table.h src/frontend/tf2mt.def src/common/unix_calls.h build/gen/stubs.S build/gen/ifaces.h build/gen/methods.h build/tf2mt/libntdll_wine.a
	@mkdir -p $(@D)
	$(MINGW) $(PECFLAGS) $(PELDFLAGS) -o $@ src/frontend/device.c src/frontend/adapter.c build/gen/stubs.S src/frontend/tf2mt.def build/tf2mt/libntdll_wine.a
	python3 tools/wine/mark_builtin.py $@
build/tf2mt/d3d9.dll: src/frontend/d3d9_forward.c src/frontend/d3d9_forward.def
	@mkdir -p $(@D)
	$(MINGW) $(PECFLAGS) $(PELDFLAGS) -o $@ $^
UNIXCXX = clang++ -arch x86_64 -mmacosx-version-min=13.0 -std=c++20 -O2 -Wall -fobjc-arc
build/tf2mt/tf2mt.so: src/unixlib/tf2mt_unix.m src/unixlib/resources.mm src/unixlib/render.mm src/unixlib/backend.h src/common/unix_calls.h src/common/commands.h $(XLATE_DEP)
	@mkdir -p $(@D)
	$(UNIXCC) -O2 -Wall -fobjc-arc -c -o build/tf2mt/tf2mt_unix.o src/unixlib/tf2mt_unix.m
	$(UNIXCXX) -c -o build/tf2mt/resources.o src/unixlib/resources.mm
	$(UNIXCXX) -c -o build/tf2mt/render.o src/unixlib/render.mm
	$(UNIXCXX) -c -o build/tf2mt/sm_decode.o src/translate/sm_decode.cpp
	$(UNIXCXX) -c -o build/tf2mt/msl_emit.o src/translate/msl_emit.cpp
	clang++ -arch x86_64 -mmacosx-version-min=13.0 -dynamiclib -install_name @rpath/tf2mt.so -framework Metal -framework QuartzCore -framework Foundation -o $@ build/tf2mt/tf2mt_unix.o build/tf2mt/resources.o build/tf2mt/render.o build/tf2mt/sm_decode.o build/tf2mt/msl_emit.o
	codesign -f -s - $@ >/dev/null 2>&1

.PHONY: frontend unixlib

replay: build/tools/replay.exe
build/tools/replay.exe: tools/replay/replay.c tools/trace/t9.h
	@mkdir -p $(@D)
	$(MINGW) -O2 -std=c17 -Wall -Wextra -Wno-unused-parameter -o $@ $< -lgdi32

probe: build/tools/probe.exe
build/tools/probe.exe: tools/census/probe.c
	@mkdir -p $(@D)
	$(MINGW) -O2 -std=c17 -Wall -Wextra -Wno-unused-parameter -o $@ $<

# ---------------------------------------------------------------- shader translator (M4)
# Tools and tests run natively (arm64); the translator itself also builds for x86_64 because it ships in tf2mt.so.
NATIVECXX = clang++ -std=c++20 -O2 -Wall -Wextra -Wno-missing-field-initializers
XLATE_SRC = src/translate/sm_decode.cpp src/translate/msl_emit.cpp
XLATE_DEP = $(XLATE_SRC) src/translate/sm.h src/translate/msl.h src/translate/msl_abi.h src/translate/msl_prelude.inc
REF_DEP = src/translate/interp.cpp src/translate/interp.h tools/translate/harness.h
METALFW = -fobjc-arc -framework Metal -framework Foundation

translate-tools: build/tools/smstat build/tools/mslbatch build/tools/difftest build/tools/opcodes build/translate/x86_64.o
build/tools/smstat: tools/translate/smstat.cpp src/translate/sm_decode.cpp src/translate/sm.h
	@mkdir -p $(@D)
	$(NATIVECXX) -o $@ tools/translate/smstat.cpp src/translate/sm_decode.cpp
build/tools/mslbatch: tools/translate/mslbatch.mm $(XLATE_DEP)
	@mkdir -p $(@D)
	$(NATIVECXX) $(METALFW) -o $@ tools/translate/mslbatch.mm $(XLATE_SRC)
build/tools/difftest: tools/translate/difftest.mm $(XLATE_DEP) $(REF_DEP)
	@mkdir -p $(@D)
	$(NATIVECXX) $(METALFW) -o $@ tools/translate/difftest.mm src/translate/interp.cpp $(XLATE_SRC)
build/tools/opcodes: tests/unit/opcodes.mm $(XLATE_DEP) $(REF_DEP)
	@mkdir -p $(@D)
	$(NATIVECXX) $(METALFW) -o $@ tests/unit/opcodes.mm src/translate/interp.cpp $(XLATE_SRC)
build/translate/x86_64.o: $(XLATE_DEP)
	@mkdir -p $(@D)
	clang++ -arch x86_64 -mmacosx-version-min=13.0 -std=c++20 -O2 -Wall -Wextra -c -o build/translate/sm_decode.o src/translate/sm_decode.cpp
	clang++ -arch x86_64 -mmacosx-version-min=13.0 -std=c++20 -O2 -Wall -Wextra -c -o build/translate/msl_emit.o src/translate/msl_emit.cpp
	ld -r -arch x86_64 -o $@ build/translate/sm_decode.o build/translate/msl_emit.o

# M4 acceptance: golden opcode tests, GPU-vs-CPU differential test and Metal compile of the live corpus.
# Full corpus (corpus/vcs, ~320k shaders, ~1 h): build/tools/mslbatch --scope all corpus corpus/vcs
test-translate: translate-tools
	build/tools/opcodes
	build/tools/difftest corpus
	build/tools/mslbatch --link 500 corpus

.PHONY: translate-tools test-translate
