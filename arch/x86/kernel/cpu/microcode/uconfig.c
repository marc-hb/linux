// SPDX-License-Identifier: GPL-2.0-or-later
#define pr_fmt(fmt) "microcode: " fmt
#include <linux/debugfs.h>
#include "internal.h"
#include "uconfig.h"

static u8 version = 2;

struct uconfig ucfg;

/*
 * Duplicate apply_microcode_late() to emulate apply() callback except
 * for triggering the hardware update. This is the exact code path for
 * sibling threads.
 *
 * With the enforcement options, see uconfig_validate_rev(), this is
 * necessary to distinguish the two separate paths between primary
 * threads and siblings.
 *
 * Thus, please make sure this update() path in sync with any change in
 * the apply() path.
 */
enum ucode_state uconfig_update_cpudata_only(int cpu)
{
	struct ucode_cpu_info *uci = ucode_cpu_info + cpu;

	if (WARN_ON_ONCE(smp_processor_id() != cpu))
		return UCODE_ERROR;

	uci->cpu_sig.rev = intel_get_microcode_revision();

	cpu_data(cpu).microcode = uci->cpu_sig.rev;
	if (!cpu)
		boot_cpu_data.microcode = uci->cpu_sig.rev;

	return UCODE_OK;
}

static struct dentry *ucfg_dentry;

#define EXPORT_KNOB(b, c)	debugfs_create_bool((c), 0644, ucfg_dentry, &(b));

static int __init uconfig_init(void)
{

	/*
	 * The following debugfs facilitates to stress late loading for
	 * Intel-internal validation use:
	 */
	ucfg_dentry = debugfs_create_dir("microcode", NULL);
	if (!ucfg_dentry)
		return -EBADFD;

	debugfs_create_u8("abi_version", 0444, ucfg_dentry, &version);

	/*
	 * Define the default behaiovr which aligns with the mainline.
	 */
	ucfg.loading	= true;
	ucfg.staging	= true;
	ucfg.anyrev	= false;
	pr_info("default configs: (loading, staging, anyrev) = (Y, Y, N)\n");

	EXPORT_KNOB(ucfg.staging,  "staging");
	EXPORT_KNOB(ucfg.loading,  "loading");
	EXPORT_KNOB(ucfg.anyrev,   "anyrev");
	return 0;
}
late_initcall(uconfig_init);

