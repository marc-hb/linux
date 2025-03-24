/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _X86_MICROCODE_INTEL_CONFIG_H
#define _X86_MICROCODE_INTEL_CONFIG_H

#include <asm/cpu.h>
#include <asm/microcode.h>

#ifdef CONFIG_MICROCODE_LATE_LOADING_DEBUG

struct uconfig {
	bool staging;	/* control whether staging or not */
	bool loading;	/* control whether loading or not */
	bool anyrev;	/* relax to load *any* revision id */
};

extern struct uconfig ucfg;

static inline bool uconfig_loading(void)
{
	return ucfg.loading;
}

static inline bool uconfig_staging(void)
{
	return ucfg.staging;
}

static inline bool uconfig_anyrev(void)
{
	return ucfg.anyrev;
}

static inline bool uconfig_validate_rev(u32 cur_rev, u32 next_rev)
{
	if (ucfg.anyrev)
		return true;

	return cur_rev < next_rev;
}

enum ucode_state uconfig_update_cpudata_only(int cpu);

#else

static inline bool uconfig_loading(void) { return true; }
static inline bool uconfig_staging(void) { return true; }
static inline bool uconfig_anyrev(void) { return false; }
static inline bool uconfig_validate_rev(u32 cur_rev, u32 next_rev) { return cur_rev < next_rev; }
static inline enum ucode_state uconfig_update_cpudata_only(int cpu) { return UCODE_ERROR; }

#endif // CONFIG_MICROCODE_LATE_LOADING_DEBUG

#endif /* _X86_MICROCODE_INTEL_CONFIG_H */
