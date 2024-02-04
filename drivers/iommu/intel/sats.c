// SPDX-License-Identifier: GPL-2.0
/*
 * sats.c - Secure ATS implementation
 *
 * Copyright (C) 2024 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 * Author: Yi Liu <yi.l.liu@intel.com>
 */

#define pr_fmt(fmt)	"DMAR: " fmt

#include <linux/bitops.h>
#include <linux/dmar.h>
#include <linux/iommu.h>
#include <linux/memory.h>
#include <linux/pci.h>
#include <linux/pci-ats.h>
#include <linux/spinlock.h>

#include "iommu.h"
#include "sats.h"
#include "trace.h"
#include "../iommu-pages.h"

int sats_hpte_dis_read = 0;
int sats_hpte_dis_write = 0;

/*
 * HPT root table allocation:
 */
struct hpt_table *intel_sats_alloc_hpt_table(struct dmar_domain *domain)
{
	int order = domain_hpt_order(domain);
	struct hpt_table *hpt_table;
	struct page *pages;

	hpt_table = kzalloc(sizeof(*hpt_table), GFP_KERNEL);
	if (!hpt_table)
		return NULL;

	pages = alloc_pages_node(domain->nid, GFP_KERNEL | __GFP_ZERO, order);
	if (!pages) {
		kfree(hpt_table);
		return NULL;
	}

	hpt_table->table	= page_address(pages);
	hpt_table->order	= order;
	hpt_table->nid		= domain->nid;
	hpt_table->coherent	= domain->iommu_coherency;
	hpt_table->domain	= domain;
	spin_lock_init(&hpt_table->lock);

	return hpt_table;
}

static void hpt_free_level_page(struct hpt_pte *parent, int level)
{
	struct hpt_pte *pte;
	int i;

	if (level == HPTL1)
		return;

	for (i = 0; i < HPT_ENTRIES; i++) {
		pte = &parent[i];
		if (!hpt_pte_address_valid(pte))
			continue;

		hpt_free_level_page(phys_to_virt(hpt_pte_addr(pte)), level - 1);
	}

	iommu_free_page(parent);
}

void intel_sats_free_hpt_table(struct hpt_table *hpt_table)
{
	struct hpt_pte *parent;
	int i, npages;

	npages = 1 << hpt_table->order;
	for (i = 0; i < npages; i++) {
		parent = hpt_table->table + VTD_PAGE_SIZE * i;
		hpt_free_level_page(parent, HPTL4);
	}

	free_pages((unsigned long)hpt_table->table, hpt_table->order);
	kfree(hpt_table);
}

/*
 * Manipulate the HPT page table entries:
 */
static struct hpt_pte *get_hpt_entry(struct hpt_table *hpt_table,
				     unsigned long phys_pfn,
				     int target_level, bool alloc)
{
	struct hpt_pte *parent = hpt_table->table, *pte;
	int level = HPTL4, offset;
	void *page;

	while (level > target_level) {
		offset = hpt_level_offset(phys_pfn, level);
		pte = &parent[offset];

		if (!hpt_pte_address_valid(pte)) {
			if (!alloc)
				return NULL;

			page = iommu_alloc_page_node(hpt_table->nid, GFP_ATOMIC);
			if (!page)
				return NULL;

			hpt_clflush(hpt_table, page, VTD_PAGE_SIZE);
			if (cmpxchg64(&pte->high, 0ULL,
				      (u64)virt_to_phys(page) |
				      HPT_ADDRESS_VALID))
				iommu_free_page(page);
			else
				hpt_clflush(hpt_table, pte, sizeof(*pte));
		}

		parent = phys_to_virt(hpt_pte_addr(pte));
		level--;
	}

	offset = hpt_level_offset(phys_pfn, level);
	return &parent[offset];
}

static void set_hpt_entry(struct hpt_table *hpt_table, struct hpt_pte *pte,
			  unsigned long phys_pfn, int level, int prot)
{
	/*
	 * Level 4: N/A
	 * Level 3: 32:30
	 * Level 2: 24:21
	 * Level 1: 16:12
	 */
	int offset = (phys_pfn >> (9 * (level - 1))) &
					GENMASK_ULL(5 - level, 0);

	hpt_set_pte_ppi(pte, offset, prot);
	trace_hpt_update(phys_pfn, prot, level, pte->low, pte->high);
}

/*
 * HPT cache synchronization
 */
static void __hpt_cache_inv_psi(struct intel_iommu *iommu,
				unsigned long phys_pfn,
				size_t pgsize, u16 did)
{
	int level = hpt_pgsize_to_level(pgsize);
	struct qi_desc desc;

	desc.qw0 = QI_HPT_TYPE | QI_HPT_DID(did) | QI_HPT_GRAN(QI_HPT_PSI);
	desc.qw1 = QI_EIOTLB_ADDR(phys_pfn << VTD_PAGE_SHIFT) |
			QI_HPT_AM(__ffs(pgsize)) | QI_EIOTLB_IH(level - 1);
	desc.qw2 = 0;
	desc.qw3 = 0;

	qi_submit_sync(iommu, &desc, 1, 0);
}

static void hpt_cache_inv_psi(struct hpt_table *hpt_table,
			      unsigned long phys_pfn, size_t pgsize)
{
	struct dmar_domain *domain = hpt_table->domain;
	struct iommu_domain_info *info;
	struct intel_iommu *iommu;
	unsigned long i;
	u16 did;

	xa_for_each(&domain->iommu_array, i, info) {
		iommu = info->iommu;

		if (!ecap_hpts(iommu->ecap))
			continue;

		did = domain_id_iommu(domain, iommu);
		__hpt_cache_inv_psi(iommu, phys_pfn, pgsize, did);
	}
}

/*
 * Manipulate the HPT mappings:
 */
int intel_sats_map_hpt(struct hpt_table *hpt_table,
		       unsigned long phys_pfn, size_t pgsize, int prot)
{
	struct hpt_pte *pte;
	int level;

	prot &= (DMA_PTE_READ | DMA_PTE_WRITE);
	level = hpt_pgsize_to_level(pgsize);
	pte = get_hpt_entry(hpt_table, phys_pfn, level, true);
	if (!pte)
		return -ENOMEM;

	if (sats_hpte_dis_read) {
		printk_once("Disable read permission to trigger HPT faults(0xa5).\n");
		prot &= ~DMA_PTE_READ;
	}
	if (sats_hpte_dis_write) {
		printk_once("Disable write permission to trigger HPT faults(0xa4).\n");
		prot &= ~DMA_PTE_WRITE;
	}

	spin_lock(&hpt_table->lock);
	set_hpt_entry(hpt_table, pte, phys_pfn, level, prot);
	spin_unlock(&hpt_table->lock);

	hpt_clflush(hpt_table, pte, sizeof(*pte));
	hpt_cache_inv_psi(hpt_table, phys_pfn, pgsize);
	trace_hpt_map(phys_pfn, pgsize, prot);

	return 0;
}

static void __sats_unmap_hpt(struct hpt_table *hpt_table, struct hpt_pte *pte,
			     int level, unsigned long phys_pfn, size_t pgsize)
{
	spin_lock(&hpt_table->lock);
	set_hpt_entry(hpt_table, pte, phys_pfn, level, 0);
	spin_unlock(&hpt_table->lock);

	hpt_clflush(hpt_table, pte, sizeof(*pte));
	hpt_cache_inv_psi(hpt_table, phys_pfn, pgsize);
	trace_hpt_unmap(phys_pfn, pgsize);
}

void intel_sats_unmap_hpt(struct hpt_table *hpt_table,
			  unsigned long ioaddr, size_t pgsize)
{
	unsigned long paddr, phys_pfn;
	struct hpt_pte *pte;
	int level;

	paddr = iommu_iova_to_phys(&hpt_table->domain->domain, ioaddr);
	phys_pfn = paddr >> VTD_PAGE_SHIFT;

	level = hpt_pgsize_to_level(pgsize);
	pte = get_hpt_entry(hpt_table, phys_pfn, level, false);
	if (!pte)
		return;

	if ((level == HPTL1) || !hpt_pte_address_valid(pte)) {
		__sats_unmap_hpt(hpt_table, pte, level, phys_pfn, pgsize);
	} else { /* Walk through the lower level. */
		size_t size = pgsize, lpgsize;

		lpgsize = hpt_level_to_pgsize(--level);

		do {
			intel_sats_unmap_hpt(hpt_table, ioaddr, lpgsize);

			ioaddr += lpgsize;
			size -= lpgsize;
		} while (size);
	}
}
