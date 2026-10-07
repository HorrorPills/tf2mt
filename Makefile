# tf2mt — top-level build (plain make, see PLAN.md D7)
MINGW   ?= /opt/homebrew/bin/x86_64-w64-mingw32-gcc
D3D9_H  ?= $(shell find /opt/homebrew/Cellar/mingw-w64 -path '*x86_64-w64-mingw32/include/d3d9.h' | head -1)
PECFLAGS = -O2 -std=c17 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Ibuild/gen
PELDFLAGS = -shared -static-libgcc -Wl,--enable-stdcall-fixup -s

all: trace null tools-native

build/gen/methods.h build/gen/thunks.S build/gen/ifaces.h build/gen/stubs.S: tools/trace/gen_methods.py
	python3 $< $(D3D9_H) build/gen

trace: build/trace/d3d9.dll
build/trace/d3d9.dll: tools/trace/trace.c tools/trace/d3d9.def build/gen/thunks.S build/gen/methods.h
	@mkdir -p $(@D)
	$(MINGW) $(PECFLAGS) $(PELDFLAGS) -o $@ tools/trace/trace.c build/gen/thunks.S tools/trace/d3d9.def

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
