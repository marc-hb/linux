/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X86_MICROCODE_INTEL_CONFIG_H
#define _X86_MICROCODE_INTEL_CONFIG_H

#include <asm/cpu.h>
#include <asm/microcode.h>

static inline bool uconfig_loading(void) { return true; }
static inline bool uconfig_staging(void) { return true; }
static inline bool uconfig_anyrev(void) { return false; }

static inline bool uconfig_validate_rev(u32 cur_rev, u32 next_rev) { return cur_rev < next_rev; }
static inline enum ucode_state uconfig_update_cpudata_only(int cpu) { return UCODE_ERROR; }

#endif /* _X86_MICROCODE_INTEL_CONFIG_H */
