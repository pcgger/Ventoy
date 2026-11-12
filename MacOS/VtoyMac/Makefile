# Build VtoyMac.kext on macOS (Xcode command line tools). Produces build/VtoyMac.kext
SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
KERNHDR  := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
CC       := $(shell xcrun -f clang)
CXX      := $(shell xcrun -f clang++)
ARCH     := x86_64
MINOS    := 11.0

COMMON   := -arch $(ARCH) -mmacosx-version-min=$(MINOS) -isysroot $(SDK) \
            -I$(KERNHDR) -O2 -Wall -Werror \
            -fno-builtin -fno-common -mkernel -fno-stack-protector -mno-red-zone \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
CXXFLAGS := $(COMMON) -nostdinc++ -std=c++17 -fapple-kext -fno-exceptions -fno-rtti
CFLAGS   := $(COMMON) -std=c11
LDFLAGS  := -arch $(ARCH) -mmacosx-version-min=$(MINOS) -isysroot $(SDK) \
            -nostdlib -Xlinker -kext -lkmod -lkmodc++ -lcc_kext

OUT  := build/VtoyMac.kext/Contents
OBJS := build/VtoyMac.o build/VtoyMac_info.o

all: $(OUT)/MacOS/VtoyMac $(OUT)/Info.plist

build/VtoyMac.o: VtoyMac.cpp VtoyMac.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

# kmod_info (what Xcode generates as <target>_info.c); OpenCore refuses kexts without _kmod_info
build/VtoyMac_info.o: VtoyMac_info.c
	mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/MacOS/VtoyMac: $(OBJS)
	mkdir -p $(OUT)/MacOS
	$(CXX) $(LDFLAGS) $(OBJS) -o $@

$(OUT)/Info.plist: Info.plist
	mkdir -p $(OUT)
	cp $< $@

clean:
	rm -rf build
