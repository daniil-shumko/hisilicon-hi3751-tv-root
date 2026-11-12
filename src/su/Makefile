# Build su + ktvsud for 32-bit ARM Android.
#
# Requires the Android NDK (r25+). Override NDK_ROOT if it is not in the
# default SDK location. The reference device is armeabi-v7a only
# (ro.zygote=zygote32), so a 64-bit build will not run there.
#
#   make            build both binaries into ./out
#   make clean

NDK_ROOT ?= $(HOME)/Library/Android/sdk/ndk
NDK      := $(shell ls -d $(NDK_ROOT)/*/ 2>/dev/null | sort -V | tail -1)
ifeq ($(NDK),)
$(error Android NDK not found under $(NDK_ROOT); set NDK_ROOT=/path/to/ndk)
endif

HOST     := $(shell uname | tr 'A-Z' 'a-z')
ifeq ($(HOST),darwin)
HOSTTAG  := darwin-x86_64
else
HOSTTAG  := linux-x86_64
endif

CC       := $(NDK)toolchains/llvm/prebuilt/$(HOSTTAG)/bin/armv7a-linux-androideabi21-clang
CFLAGS   := -O2 -Wall -Wextra -Wno-unused-parameter
OUT      := out

all: $(OUT)/su $(OUT)/ktvsud

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/su: su.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/ktvsud: ktvsud.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: all clean
