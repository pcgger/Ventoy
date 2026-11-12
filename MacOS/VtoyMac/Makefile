# Build VtoyMac.kext on macOS (Xcode command line tools). Produces build/VtoyMac.kext
SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
KERNHDR  := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
CXX      := $(shell xcrun -f clang++)
ARCH     := x86_64
MINOS    := 11.0

CXXFLAGS := -arch $(ARCH) -mmacosx-version-min=$(MINOS) -isysroot $(SDK) \
            -I$(KERNHDR) -nostdinc++ -std=c++17 -O2 -Wall -Werror \
            -fno-builtin -fno-common -mkernel -fapple-kext -fno-exceptions -fno-rtti \
            -fno-stack-protector -mno-red-zone -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE \
            -DAPPLE -DNeXT
LDFLAGS  := -arch $(ARCH) -mmacosx-version-min=$(MINOS) -isysroot $(SDK) \
            -nostdlib -Xlinker -kext -lkmod -lkmodc++ -lcc_kext

OUT := build/VtoyMac.kext/Contents

all: $(OUT)/MacOS/VtoyMac $(OUT)/Info.plist

build/VtoyMac.o: VtoyMac.cpp VtoyMac.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/MacOS/VtoyMac: build/VtoyMac.o
	mkdir -p $(OUT)/MacOS
	$(CXX) $(LDFLAGS) $< -o $@

$(OUT)/Info.plist: Info.plist
	mkdir -p $(OUT)
	cp $< $@

clean:
	rm -rf build
