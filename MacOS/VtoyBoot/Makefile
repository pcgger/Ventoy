# Build VtoyBoot.efi (UEFI application, x86_64 PE32+) with clang + lld-link; no EDK2 needed.
# CC / LD default to Homebrew LLVM on macOS runners; override on other hosts.
CC  ?= $(shell ls /usr/local/opt/llvm/bin/clang /opt/homebrew/opt/llvm/bin/clang 2>/dev/null | head -1)
LD  ?= $(shell ls /usr/local/opt/lld/bin/lld-link /opt/homebrew/opt/lld/bin/lld-link /usr/local/opt/llvm/bin/lld-link 2>/dev/null | head -1)

CFLAGS  := -target x86_64-unknown-windows -ffreestanding -fshort-wchar -fno-stack-protector -fno-builtin \
           -mno-red-zone -mno-stack-arg-probe -O2 -Wall -Werror -std=c11
LDFLAGS := /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64

build/VtoyBoot.efi: vtoyboot.c efi.h
	mkdir -p build
	$(CC) $(CFLAGS) -c vtoyboot.c -o build/vtoyboot.o
	$(LD) $(LDFLAGS) /out:$@ build/vtoyboot.o

clean:
	rm -rf build
