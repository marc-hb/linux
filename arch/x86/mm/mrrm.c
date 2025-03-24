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
#include <asm/resctrl.h>

struct acpi_mrrm_mem_range_entry *mrrm_mem_range_entry;
u32 mrrm_mem_entry_num;
u32 erdt_max_clos;
struct enhanced_rdt_para enhanced_rdt;

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
	enhanced_rdt.max_mem_region = acpi_mrrm->max_mem_region;
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

#define ERDT_TYPE_RMDD	0
#define ERDT_TYPE_CACD	1
#define ERDT_TYPE_MMRC	4
#define ERDT_TYPE_MARC	5

static __init void
mmrc_enumerate(struct acpi_table_erdt_mmrc *mmrc)
{
	struct mmrc_para *p;

	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)mmrc->base);
	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)mmrc->size);
	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)mmrc->width);
	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)mmrc->upscaling_factor);
	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)mmrc->correction_factor_list_length);

	p = &enhanced_rdt.mmrc;
	p->base = (void *)mmrc->base;
	p->size = mmrc->size;
	p->width = mmrc->width;
	p->upscaling_factor = mmrc->upscaling_factor;
	p->correction_factor_list_length = mmrc->correction_factor_list_length;
}

static __init void
marc_enumerate(struct acpi_table_erdt_marc *marc)
{
	struct marc_para *p;

	p = &enhanced_rdt.marc;
	p->flags = marc->flags;
	p->opt_bw_base = marc->opt_bw_base;
	p->min_bw_base = marc->min_bw_base;
	p->max_bw_base = marc->max_bw_base;
	p->size = marc->size;
	p->range = marc->range;

	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)p->opt_bw_base);
	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)p->min_bw_base);
	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)p->max_bw_base);
	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)p->flags);
	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)p->size);
	pr_info("%s: %d, %lx\n", __func__, __LINE__, (unsigned long)p->range);
}

#define RMDD_FLAGS_L3_DOMAIN	1

#define RMDD_TYPE_MMRC	4
#define RMDD_TYPE_MARC	5

static bool enumerate_mmrc;
static bool enumerate_marc;

static __init int rmdd_enumerate(struct acpi_table_erdt_rmdd *rmdd)
{
	struct acpi_table_erdt_rmdd_reg *rmdd_reg;
	void *rmdd_end;
	u64 *rdt_ctrl;

	if ((rmdd->flags & RMDD_FLAGS_L3_DOMAIN) == 0)
		return -EOPNOTSUPP;

	pr_info("max rmids: %d\n", (unsigned int)rmdd->max_rmids);
	pr_info("base: 0x%lx\n", (unsigned long)rmdd->ctrl_reg_base);
	pr_info("size: %d\n", (unsigned int)rmdd->ctrl_reg_size);
	pr_info("flags: %d\n", rmdd->flags);
	pr_info("domain id: %d\n", rmdd->domain_id);

	rdt_ctrl = ioremap(rmdd->ctrl_reg_base, rmdd->ctrl_reg_size);
	if (!rdt_ctrl) {
		pr_err("Cannot map RDT ctrl register %lx\n",
		       (unsigned long)rmdd->ctrl_reg_base);

		return -ENOMEM;
	}
	enhanced_rdt.rdt_ctrl = rdt_ctrl;

	rmdd_reg = (struct acpi_table_erdt_rmdd_reg *)((void *)rmdd +
		    sizeof(struct acpi_table_erdt_rmdd));
	rmdd_end = (void *)rmdd + rmdd->header.length - 1;

	while ((void *)rmdd_reg <= rmdd_end) {
		switch (rmdd_reg->type) {
		case RMDD_TYPE_MMRC:
		//	rmdd_mmrc_enumerate((void *)rmdd_reg);
			enumerate_mmrc = true;
			break;
		case RMDD_TYPE_MARC:
	//		rmdd_marc_enumerate((void *)rmdd_reg);
			enumerate_marc = true;
			break;
		default:
		}
		rmdd_reg++;
	}

	return 0;
}

static int cacd_enumerate(struct acpi_table_erdt_cacd *cacd)
{
	u32 *enumeration_id;
	int i = 0;

	pr_info("%s: %d\n", __func__, __LINE__);
	pr_info("cacd=%lx\n", (unsigned long)cacd);
	pr_info("length=%d\n", cacd->header.length);
	pr_info("domainid=%d\n", cacd->domainid);
	enumeration_id = (void *)cacd + 8;
	while (enumeration_id < ((u32 *)cacd + cacd->header.length)) {
		pr_info("enumeration_id[%d]=%x, %lx\n", i++, *enumeration_id, (unsigned long)enumeration_id);
		enumeration_id++;
	}

	return 0;
}

static __init int erdt_enumerate(struct acpi_table_erdt *erdt,
				 bool enumerate_rmdd_only)
{
	struct acpi_table_erdt_sub_structure *erdt_sub;
	void *erdt_end;
	int sub_strucutres = 0;

	enhanced_rdt.max_clos = erdt->max_clos;
	erdt_sub = (struct acpi_table_erdt_sub_structure *)((void *)erdt +
		   sizeof(struct acpi_table_erdt));
	erdt_end = (void *)erdt + erdt->header.length - 1;
	while (((void *)erdt_sub < erdt_end) && erdt_sub->length) {
		switch (erdt_sub->type) {
		case ERDT_TYPE_RMDD:
			rmdd_enumerate((void *)erdt_sub);
			break;
		case ERDT_TYPE_CACD:
			cacd_enumerate((void *)erdt_sub);
			break;
		case ERDT_TYPE_MMRC:
			mmrc_enumerate((void *)erdt_sub);
			break;
		case ERDT_TYPE_MARC:
			marc_enumerate((void *)erdt_sub);
			break;
		default:
		}
		pr_info("erdt sub_structures[%d]: type=%d\n", sub_strucutres, erdt_sub->type);
		erdt_sub = (void *)erdt_sub + erdt_sub->length;
		sub_strucutres++;
	}

	return 0;
}

static __init int acpi_parse_erdt(struct acpi_table_header *table)
{
	struct acpi_table_erdt *erdt = NULL;

	pr_info("%s: %d\n", __func__, __LINE__);
	erdt = (struct acpi_table_erdt *)table;
	if (!erdt)
		return -ENODEV;

	pr_info("%s: %d\n", __func__, __LINE__);
	/* Enumerate RMDD */
	erdt_enumerate(erdt, true);
	/* Enumerate MMRC and MARC */
//	erdt_enumerate(erdt, false);

	*enhanced_rdt.rdt_ctrl &= ~0x4;
	enhanced_rdt.valid = true;

	pr_info("rdt_ctrl: 0x%lx\n", (unsigned long)*enhanced_rdt.rdt_ctrl);

	return 0;
}

static __init int mrrm_init(void)
{
	acpi_table_parse(ACPI_SIG_MRRM, acpi_parse_mrrm);
	acpi_table_parse(ACPI_SIG_ERDT, acpi_parse_erdt);

	return 0;
}
subsys_initcall(mrrm_init);
