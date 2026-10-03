# Builds libVkLayer_kettle_framegen.so; the shaders are compiled to SPIR-V and embedded.
#   make
#   make install DESTDIR=... PREFIX=/usr
#   make lib32 && make install-lib32   (32-bit x86 copy for 32-bit games; needs multilib gcc)
#   make CFLAGS=-I/path/to/Vulkan-Headers/include   (without system vulkan-headers)
#   make test          run test/cases through the shaders, compare with their references
#   make test-update   make the current output the references (review the images first)
CC ?= cc
GLSLANG ?= glslangValidator
CFLAGS ?= -O2
CFLAGS32 ?= -m32
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
LIB32DIR ?= $(PREFIX)/lib32
LAYERDIR := $(PREFIX)/share/vulkan/implicit_layer.d
SHADERS := luma0 down motion filter still synth
HEADERS := $(SHADERS:%=build/%.spv.h)
LIB := build/libVkLayer_kettle_framegen.so
LIB32 := build/32/libVkLayer_kettle_framegen.so
FGTEST := build/fgtest
CASES ?= $(wildcard test/cases/*)
# $(call build,extra flags)
build = $(CC) $(CFLAGS) $(1) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -fPIC -shared -fvisibility=hidden \
	  -Ibuild $(LDFLAGS) -o $@ framegen.c -lpthread -lm

all: $(LIB)

lib32: $(LIB32)

build/%.spv.h: shaders/%.comp
	@mkdir -p build
	$(GLSLANG) -V --target-env vulkan1.0 --vn spv_$* -o $@ $<

$(LIB): framegen.c shaders.h $(HEADERS)
	$(call build)

$(LIB32): framegen.c shaders.h $(HEADERS)
	@mkdir -p build/32
	$(call build,$(CFLAGS32))

$(FGTEST): test/fgtest.c shaders.h $(HEADERS)
	$(CC) $(CFLAGS) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -I. -Ibuild $(LDFLAGS) -o $@ test/fgtest.c \
	  -lvulkan -lm

test: $(FGTEST)
	$(FGTEST) $(FGTEST_FLAGS) $(CASES)

test-update: $(FGTEST)
	$(FGTEST) -u $(CASES)

install: $(LIB)
	install -Dm755 $(LIB) -t $(DESTDIR)$(LIBDIR)
	@mkdir -p $(DESTDIR)$(LAYERDIR)
	sed 's|/usr/lib/|$(LIBDIR)/|' VkLayer_kettle_framegen.json \
	  >$(DESTDIR)$(LAYERDIR)/VkLayer_kettle_framegen.json

# The loader matches layers by name, so the 32-bit manifest declares its own. With one name in
# both manifests the loader keeps one of them and the other bitness runs without the layer.
install-lib32: $(LIB32)
	install -Dm755 $(LIB32) -t $(DESTDIR)$(LIB32DIR)
	@mkdir -p $(DESTDIR)$(LAYERDIR)
	sed -e 's|/usr/lib/|$(LIB32DIR)/|' -e 's|"VK_LAYER_KETTLE_framegen"|"VK_LAYER_KETTLE_framegen_32"|' \
	  VkLayer_kettle_framegen.json >$(DESTDIR)$(LAYERDIR)/VkLayer_kettle_framegen_32.json

clean:
	rm -rf build

.PHONY: all lib32 install install-lib32 clean test test-update
