/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sats.h - secure ATS header
 *
 * Copyright (C) 2024 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 * Author: Yi Liu <yi.l.liu@intel.com>
 */

#ifndef __INTEL_ATS_H
#define __INTEL_ATS_H

#define HPT_PFN_SHIFT		(5)
#define HPT_LEVEL_STRIDE	(8)
#define HPT_LEVEL_MASK		(BIT_ULL(HPT_LEVEL_STRIDE) - 1)
#define HPT_ADDRESS_VALID	BIT_ULL(0)

#define	HPT_ENTRIES		(256)

extern int sats_hpte_dis_read, sats_hpte_dis_write;

enum hpt_level {
	HPTL1	= 1,
	HPTL2,
	HPTL3,
	HPTL4
};

struct hpt_table {
	void			*table;		/* Top table */
	struct dmar_domain	*domain;	/* DMA translation table */
	int			order;		/* Top table page order */
	int			nid;		/* Memory node id */
	spinlock_t		lock;		/* Protect HPT update */
	unsigned long		coherent:1;	/* Hardware accesses to HPT
						 * entries snoop processor
						 * caches
						 */
};

struct hpt_pte {
	u64		low;
	u64		high;
};

static inline bool hpt_pte_address_valid(struct hpt_pte *pte)
{
	return pte->high & HPT_ADDRESS_VALID;
}

static inline u64 hpt_pte_addr(struct hpt_pte *pte)
{
	return pte->high & GENMASK_ULL(51, 12);
}

static inline int domain_hpt_order(struct dmar_domain *domain)
{
	return domain->gaw < 48 ? 0 : domain->gaw - 48;
}

/*
 * Level 4: HAW-1:41
 * Level 3: 40:33
 * Level 2: 32:25
 * Level 1: 24:17
 */
static inline int hpt_level_offset(unsigned long phys_pfn, int level)
{
	int low = 17 + (level - 1) * 8 - VTD_PAGE_SHIFT;

	return (phys_pfn & GENMASK_ULL(low + 7, low)) >> low;
}

static inline void hpt_clflush(struct hpt_table *hpt_table,
			       void *addr, int size)
{
	if (!hpt_table->coherent)
		clflush_cache_range(addr, size);
}

static inline int hpt_pgsize_to_level(size_t pgsize)
{
	int level = 0;

	pgsize >>= VTD_PAGE_SHIFT;
	while (pgsize) {
		level++;
		pgsize >>= VTD_STRIDE_SHIFT;
	}

	return level;
}

static inline int hpt_level_to_pgsize(int level)
{
	return 1ULL << ((level - 1) * VTD_STRIDE_SHIFT + VTD_PAGE_SHIFT);
}

static inline void
hpt_set_pte_ppi(struct hpt_pte *pte, int offset, u64 value)
{
	int high, low;
	u64 mask;

	if (offset <= 15) {
		low = offset * 4;
		high = low + 3;
		mask = GENMASK_ULL(high, low);
		pte->low &= ~mask;
		pte->low |= (value & 0xf) << low;
	} else {
		offset -= 16;
		low = offset * 4;
		high = low + 3;
		mask = GENMASK_ULL(high, low);
		pte->high &= ~mask;
		pte->high |= (value & 0xf) << low;
	}
}

/* Return the number of pages that a HPT table entry could cover. */
static inline unsigned long long hpt_level_to_entry_coverage(int level)
{
	unsigned long long coverage = 0;

	switch (level) {
	case HPTL1:
		coverage = SZ_128K;
		break;
	case HPTL2:
		coverage = SZ_32M;
		break;
	case HPTL3:
		coverage = SZ_8G;
		break;
	case HPTL4:
		coverage = SZ_1T * 2;
		break;
	default:
		break;
	}

	return coverage >> VTD_PAGE_SHIFT;
}

struct hpt_table *intel_sats_alloc_hpt_table(struct dmar_domain *domain);
void intel_sats_free_hpt_table(struct hpt_table *hpt_table);
int intel_sats_map_hpt(struct hpt_table *hpt_table,
		       unsigned long phys_pfn, size_t pgsize, int prot);
void intel_sats_unmap_hpt(struct hpt_table *hpt_table,
			  unsigned long phys_pfn, size_t pgsize);
#endif /* __INTEL_ATS_H */
