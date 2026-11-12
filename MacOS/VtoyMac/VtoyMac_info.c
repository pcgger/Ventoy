/*
 * kmod_info for VtoyMac - the same file Xcode generates for kernel extension targets
 * ("<target>_info.c"). Boot loaders (OpenCore) and the kernel look up the _kmod_info
 * symbol to get the bundle id / version and the start/stop entry points; a kext linked
 * without it is rejected by OpenCore with "Prelinked injection ... - Invalid Parameter".
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(net.ventoy.VtoyMac, "1.0.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
