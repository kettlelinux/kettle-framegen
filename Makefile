# Builds libVkLayer_kettle_framegen.so; the shaders are compiled to SPIR-V and embedded.
#   make                                   (PKGBUILD)
#   make CFLAGS=-I/path/to/Vulkan-Headers/include   (without system vulkan-headers)
CC ?= cc
GLSLANG ?= glslangValidator
CFLAGS ?= -O2
SHADERS := luma0 down motion filter synth
HEADERS := $(SHADERS:%=build/%.spv.h)
LIB := build/libVkLayer_kettle_framegen.so

all: $(LIB)

build/%.spv.h: shaders/%.comp
	@mkdir -p build
	$(GLSLANG) -V --target-env vulkan1.0 --vn spv_$* -o $@ $<

$(LIB): framegen.c $(HEADERS)
	$(CC) $(CFLAGS) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fPIC -shared -fvisibility=hidden \
	  -Ibuild $(LDFLAGS) -o $@ framegen.c -lpthread -lm

clean:
	rm -rf build

.PHONY: all clean
