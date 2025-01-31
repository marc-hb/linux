// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024, Intel Corporation.
 *
 * Memory Range and Region Mapping (MRRM) structure
 *
 * This program parses and reports the platform's MRRM table, and registers.
 */

#define pr_fmt(fmt) "acpi/mrrm: " fmt

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/list_sort.h>
#include <linux/memregion.h>
#include <linux/memory.h>
#include <linux/mutex.h>
#include <linux/node.h>
#include <linux/sysfs.h>
#include <linux/dax.h>
#include <linux/memory-tiers.h>

struct acpi_mrrm_mem_range_entry *mrrm_mem_range_entry;
u32 mrrm_mem_entry_num;

/*
 * Fix me if Region-ID is programmed.
 * Memory range entry size is 32 assuming Region-ID cannot be programmable.
 */
#define MRRM_RANGE_ENTRY_SIZE	32

static __init int mem_range_enumerate(struct acpi_table_mrrm *acpi_mrrm)
{
	struct acpi_table_mrrm_mem_range_entry *mem_range_entry = NULL;
	int len, mrrm_entry_size;
	void *mrrm_end = NULL;

	len = acpi_mrrm->header.length - sizeof(struct acpi_table_mrrm);
	mrrm_entry_size = len / MRRM_RANGE_ENTRY_SIZE *
			  sizeof(struct acpi_mrrm_mem_range_entry);
	mrrm_mem_range_entry = kzalloc(mrrm_entry_size, GFP_KERNEL);
	if (!mrrm_mem_range_entry)
		return -ENOMEM;

	mem_range_entry = (void *)acpi_mrrm + sizeof(struct acpi_table_mrrm);
	mrrm_end = (void *)acpi_mrrm + acpi_mrrm->header.length - 1;

	pr_info("max memory region: %d\n", acpi_mrrm->max_mem_region);
	pr_info("flags: %d\n", acpi_mrrm->flags);
	while ((mrrm_end + 1) > (void *)mem_range_entry) {
		struct acpi_table_mrrm_mem_range_entry *p2;
		struct acpi_mrrm_mem_range_entry *p1;

		p1 = mrrm_mem_range_entry + mrrm_mem_entry_num;
		p2 = mem_range_entry;
		p1->base = ((u64)p2->base_addr_high << 32) + p2->base_addr_low;
		p1->length = ((u64)p2->len_high << 32) + p2->len_low;
		if (p2->region_id_flags & 0x1)
			p1->local_region_id = p2->local_region_id;
		else
			p1->local_region_id = -1;
		if (p2->region_id_flags & 0x2)
			p1->remote_region_id = p2->remote_region_id;
		else
			p1->remote_region_id = -1;

		mem_range_entry++;
		mrrm_mem_entry_num++;
	}

	return 0;
}

static __init int acpi_parse_mrrm(struct acpi_table_header *table)
{
	struct acpi_table_mrrm *mrrm = NULL;

	mrrm = (struct acpi_table_mrrm *)table;
	if (!mrrm)
		return -ENODEV;

	return mem_range_enumerate(mrrm);
}

static __init int mrrm_init(void)
{
	acpi_table_parse(ACPI_SIG_MRRM, acpi_parse_mrrm);

	return 0;
}
subsys_initcall(mrrm_init);
