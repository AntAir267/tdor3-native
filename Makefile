# TDOR3 native: build the runtime and the C generated from the user's own game files.
#   make GAME=<folder with TDOR3.exe>      translate + build build/tdor3
# Python with Capstone: the repository's .venv if there is one
PY ?= $(firstword $(wildcard .venv/bin/python) python3)
GAME ?= $(HOME)/.local/share/tdor3-native/game
BUILD_CFG ?= builds/tdor3.py
# Parallel compiles: each takes ~300 MB; keep it modest so the machine stays usable.
JOBS ?= 6
MAKEFLAGS += -j$(JOBS)
CC = gcc
# SDL3: a static build in third_party/sdl3 (third_party/README.md), else the system's (pkg-config)
SDL3 ?= $(firstword $(wildcard third_party/sdl3))
ifneq ($(SDL3),)
SDL3_CFLAGS = -I$(SDL3)/include
SDL3_LIBS = $(SDL3)/lib/libSDL3.a
else
SDL3_CFLAGS = $(shell pkg-config --cflags sdl3)
SDL3_LIBS = $(shell pkg-config --libs sdl3)
endif
CFLAGS_COMMON = -std=gnu11 -g -fno-strict-aliasing -frounding-math -fno-omit-frame-pointer -Iruntime -Igen $(SDL3_CFLAGS)
CFLAGS_GEN = $(CFLAGS_COMMON) -O2 -w -DCPU_FAST_MEM
CFLAGS_RT = $(CFLAGS_COMMON) -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-missing-field-initializers
LDFLAGS = -rdynamic $(SDL3_LIBS) -pthread -lm -ldl -lz -lvulkan $(shell pkg-config --libs libavformat libavcodec libswresample libswscale libavutil)

RT_SRC = $(wildcard runtime/*.c)
RT_OBJ = $(patsubst runtime/%.c,build/obj/rt_%.o,$(RT_SRC))
-include gen/files.mk
GEN_SRC = $(GEN_CODE) gen/table.c gen/stubs.c gen/x87_special.c
GEN_OBJ = $(patsubst gen/%.c,build/obj/gen_%.o,$(GEN_SRC))

all: build/tdor3

gen/funcs.h: $(wildcard tools/*.py) $(BUILD_CFG)
	$(PY) tools/recomp.py "$(GAME)" $(BUILD_CFG) gen
gen/table.c gen/stubs.c gen/x87_special.c gen/image.h: gen/funcs.h ;
gen/files.mk: gen/funcs.h
	@echo 'GEN_CODE = $(sort $(wildcard gen/code_*.c))' > $@

build/obj/gen_%.o: gen/%.c gen/funcs.h runtime/cpu.h | build/obj
	$(CC) $(CFLAGS_GEN) -c $< -o $@

build/obj/rt_%.o: runtime/%.c runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -c $< -o $@

# Shaders: SPIR-V headers made with glslang, kept in the repository (glslang needed only after a change)
SHADERS = $(wildcard runtime/shaders/*.vert runtime/shaders/*.frag)
SPV_H = $(addsuffix .spv.h,$(SHADERS))
GLSLANG ?= glslangValidator
ifneq ($(shell command -v $(GLSLANG) 2>/dev/null),)
runtime/shaders/%.spv.h: runtime/shaders/% runtime/shaders/push.glsl
	$(GLSLANG) --target-env vulkan1.3 --vn spv_$(subst .,_,$(notdir $<)) -o $@ $<
endif
build/obj/rt_vk.o: $(SPV_H)

build/tdor3: $(GEN_OBJ) $(RT_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

build/obj:
	mkdir -p $@

# Reference build: the original code in Unicorn on the same runtime (ref/ref_cpu.c, tools/lockstep.py).
# Unicorn: the library in the Python unicorn package (pip install unicorn), in .venv or UNICORN=<its folder>
UNICORN ?= $(firstword $(wildcard .venv/lib/python3*/site-packages/unicorn))
build/obj/ref_cpu.o: ref/ref_cpu.c runtime/*.h gen/funcs.h | build/obj
	$(CC) $(CFLAGS_RT) -I$(UNICORN)/include -c $< -o $@

build/tdor3_ref: $(GEN_OBJ) $(RT_OBJ) build/obj/ref_cpu.o
	$(CC) -o $@ $^ $(UNICORN)/lib/libunicorn.a $(LDFLAGS) -lrt

clean:
	rm -rf build gen

.PHONY: all clean
