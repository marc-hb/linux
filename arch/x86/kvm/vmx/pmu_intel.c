// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM PMU support for Intel CPUs
 *
 * Copyright 2011 Red Hat, Inc. and/or its affiliates.
 *
 * Authors:
 *   Avi Kivity   <avi@redhat.com>
 *   Gleb Natapov <gleb@redhat.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/kvm_host.h>
#include <linux/perf_event.h>
#include <asm/perf_event.h>
#include <asm/fpu/xstate.h>
#include "x86.h"
#include "cpuid.h"
#include "lapic.h"
#include "nested.h"
#include "pmu.h"

/*
 * Perf's "BASE" is wildly misleading, architectural PMUs use bits 31:16 of ECX
 * to encode the "type" of counter to read, i.e. this is not a "base".  And to
 * further confuse things, non-architectural PMUs use bit 31 as a flag for
 * "fast" reads, whereas the "type" is an explicit value.
 */
#define INTEL_RDPMC_GP		0
#define INTEL_RDPMC_FIXED	INTEL_PMC_FIXED_RDPMC_BASE

#define INTEL_RDPMC_TYPE_MASK	GENMASK(31, 16)
#define INTEL_RDPMC_INDEX_MASK	GENMASK(15, 0)

#define MSR_PMC_FULL_WIDTH_BIT      (MSR_IA32_PMC0 - MSR_IA32_PERFCTR0)

static void vmx_enable_lbr_msrs_passthrough(struct kvm_vcpu *vcpu);
static void vmx_disable_lbr_msrs_passthrough(struct kvm_vcpu *vcpu);

static void reprogram_fixed_counters(struct kvm_pmu *pmu, u64 data)
{
	u64 fixed_bits = fixed_ctrs_bitmap(pmu);
	u64 old_fixed_ctr_ctrl = pmu->fixed_ctr_ctrl;
	struct kvm_pmc *pmc;
	int i;

	pmu->fixed_ctr_ctrl = data;
	for_each_set_bit(i, (unsigned long*)&fixed_bits, KVM_MAX_NR_INTEL_FIXED_COUTNERS) {
		u8 new_ctrl = fixed_ctrl_field(data, i);
		u8 old_ctrl = fixed_ctrl_field(old_fixed_ctr_ctrl, i);

		if (old_ctrl == new_ctrl)
			continue;

		pmc = get_fixed_pmc_from_idx(pmu, i);

		__set_bit(KVM_FIXED_PMC_BASE_IDX + i, pmu->pmc_in_use);
		kvm_pmu_request_counter_reprogram(pmc);
	}
}

static inline bool intel_is_valid_pmc(struct kvm_pmu *pmu,
				      unsigned int idx, bool fixed)
{
	return fixed ? fixed_ctr_is_supported(pmu, idx)
		     : gp_ctr_is_supported(pmu, idx);
}

static struct kvm_pmc *intel_rdpmc_ecx_to_pmc(struct kvm_vcpu *vcpu,
					    unsigned int idx, u64 *mask)
{
	unsigned int type = idx & INTEL_RDPMC_TYPE_MASK;
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *counters;
	unsigned int num_counters;
	u64 bitmask;

	/*
	 * The encoding of ECX for RDPMC is different for architectural versus
	 * non-architecturals PMUs (PMUs with version '0').  For architectural
	 * PMUs, bits 31:16 specify the PMC type and bits 15:0 specify the PMC
	 * index.  For non-architectural PMUs, bit 31 is a "fast" flag, and
	 * bits 30:0 specify the PMC index.
	 *
	 * Yell and reject attempts to read PMCs for a non-architectural PMU,
	 * as KVM doesn't support such PMUs.
	 */
	if (WARN_ON_ONCE(!pmu->version))
		return NULL;

	/*
	 * General Purpose (GP) PMCs are supported on all PMUs, and fixed PMCs
	 * are supported on all architectural PMUs, i.e. on all virtual PMUs
	 * supported by KVM.  Note, KVM only emulates fixed PMCs for PMU v2+,
	 * but the type itself is still valid, i.e. let RDPMC fail due to
	 * accessing a non-existent counter.  Reject attempts to read all other
	 * types, which are unknown/unsupported.
	 */
	switch (type) {
	case INTEL_RDPMC_FIXED:
		counters = pmu->fixed_counters;
		num_counters = KVM_MAX_NR_INTEL_FIXED_COUTNERS;
		bitmask = pmu->counter_bitmask[KVM_PMC_FIXED];
		break;
	case INTEL_RDPMC_GP:
		counters = pmu->gp_counters;
		num_counters = KVM_MAX_NR_INTEL_GP_COUNTERS;
		bitmask = pmu->counter_bitmask[KVM_PMC_GP];
		break;
	default:
		return NULL;
	}

	idx &= INTEL_RDPMC_INDEX_MASK;
	if (!intel_is_valid_pmc(pmu, idx, type == INTEL_RDPMC_FIXED))
		return NULL;

	*mask &= bitmask;
	return &counters[array_index_nospec(idx, num_counters)];
}

static inline bool fw_writes_is_enabled(struct kvm_vcpu *vcpu)
{
	return (vcpu_get_perf_capabilities(vcpu) & PERF_CAP_FW_WRITES) != 0;
}

static inline bool legacy_pebs_is_enabled(struct kvm_vcpu *vcpu)
{
	int pebs_format = (vcpu_get_perf_capabilities(vcpu) &
			   PERF_CAP_PEBS_FORMAT) >> PERF_CAP_PEBS_FORMAT_SHIFT;

	return vmx_pebs_supported() && pebs_format < PERF_CAP_ARCH_PEBS_FORMAT;
}

static inline bool pebs_baseline_is_enabled(struct kvm_vcpu *vcpu)
{
	return (vcpu_get_perf_capabilities(vcpu) & PERF_CAP_PEBS_BASELINE) != 0;
}

static inline struct kvm_pmc *get_fw_gp_pmc(struct kvm_pmu *pmu, u32 msr)
{
	if (!fw_writes_is_enabled(pmu_to_vcpu(pmu)))
		return NULL;

	return get_gp_pmc(pmu, msr, MSR_IA32_PMC0);
}

static bool intel_pmu_is_valid_lbr_msr(struct kvm_vcpu *vcpu, u32 index)
{
	struct x86_pmu_lbr *records = vcpu_to_lbr_records(vcpu);
	bool ret = false;

	if (!intel_pmu_lbr_is_enabled(vcpu))
		return ret;

	if (!cpu_feature_enabled(X86_FEATURE_ARCH_LBR)) {
		if (index == MSR_LBR_SELECT || index == MSR_LBR_TOS)
			return true;
	} else if (index == MSR_ARCH_LBR_CTL || index == MSR_ARCH_LBR_DEPTH)
		return true;

	ret = (index >= records->from && index < records->from + records->nr) ||
		(index >= records->to && index < records->to + records->nr);

	if (!ret && records->info)
		ret = (index >= records->info && index < records->info + records->nr);

	return ret;
}

static inline bool intel_pmu_is_valid_extra_msr(struct kvm_vcpu *vcpu, u32 msr)
{
	return (kvm_pmu_is_possible_extra_msr(msr) &&
		kvm_mediated_pmu_enabled(vcpu) &&
		cpuid_model_is_consistent(vcpu));
}

static bool intel_is_valid_msr(struct kvm_vcpu *vcpu, u32 msr)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	u64 perf_capabilities;
	int ret;

	switch (msr) {
	case MSR_CORE_PERF_FIXED_CTR_CTRL:
		return kvm_pmu_has_perf_global_ctrl(pmu);
	case MSR_CORE_PERF_GLOBAL_STATUS_SET:
	case MSR_CORE_PERF_GLOBAL_INUSE:
		/* For now we only support version 2 and 5, so the features
		 * from v3 ~ v4 are consolidated in version 5. */
		return vcpu_to_pmu(vcpu)->version >= 5;
	case MSR_PERF_METRICS:
		return vcpu_has_perf_metrics(vcpu);
	case MSR_IA32_PEBS_ENABLE:
		ret = !pmu->arch_pebs &&
		      (vcpu_get_perf_capabilities(vcpu) & PERF_CAP_PEBS_FORMAT);
		break;
	case MSR_IA32_DS_AREA:
		ret = guest_cpu_cap_has(vcpu, X86_FEATURE_DS);
		break;
	case MSR_PEBS_DATA_CFG:
		perf_capabilities = vcpu_get_perf_capabilities(vcpu);
		ret = !pmu->arch_pebs &&
		      (perf_capabilities & PERF_CAP_PEBS_BASELINE) &&
		      ((perf_capabilities & PERF_CAP_PEBS_FORMAT) > 3);
		break;
	case MSR_IA32_PEBS_BASE:
	case MSR_IA32_PEBS_INDEX:
		ret = pmu->arch_pebs;
		break;
	case MSR_IA32_RTIT_TRIGGER0_CFG ... MSR_IA32_RTIT_TRIGGER6_CFG:
		ret = vmx_guest_has_intel_pttt(vcpu) &&
		      ((msr - MSR_IA32_RTIT_TRIGGER0_CFG) <
				to_vmx(vcpu)->pt_desc.num_trigger_msrs);
		break;
	default:
		ret = get_gp_pmc(pmu, msr, MSR_IA32_PERFCTR0) ||
			get_gp_pmc(pmu, msr, MSR_P6_EVNTSEL0) ||
			get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CTR) ||
			get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CFG_A) ||
			get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CFG_C) ||
			get_fixed_pmc(pmu, msr, MSR_CORE_PERF_FIXED_CTR0) ||
			get_fixed_pmc(pmu, msr, MSR_IA32_PMC_V6_FX0_CTR) ||
			get_fixed_pmc(pmu, msr, MSR_IA32_PMC_V6_FX0_CFG_C) ||
			get_fw_gp_pmc(pmu, msr) ||
			intel_pmu_is_valid_lbr_msr(vcpu, msr) ||
			intel_pmu_is_valid_extra_msr(vcpu, msr);
		break;
	}

	return ret;
}

static struct kvm_pmc *intel_msr_idx_to_pmc(struct kvm_vcpu *vcpu, u32 msr)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc = NULL;

	if (msr < MSR_IA32_PMC_V6_GP0_CTR) {
		pmc = get_fixed_pmc(pmu, msr, MSR_CORE_PERF_FIXED_CTR0);
		pmc = pmc ? pmc : get_gp_pmc(pmu, msr, MSR_P6_EVNTSEL0);
		pmc = pmc ? pmc : get_gp_pmc(pmu, msr, MSR_IA32_PERFCTR0);
		pmc = pmc ? pmc : get_fw_gp_pmc(pmu, msr);
	} else {
		pmc = pmc ? pmc : get_fixed_pmc(pmu, msr, MSR_IA32_PMC_V6_FX0_CTR);
		pmc = pmc ? pmc : get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CFG_A);
		pmc = pmc ? pmc : get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CTR);
	}

	return pmc;
}

static inline void intel_pmu_release_guest_lbr_event(struct kvm_vcpu *vcpu)
{
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);

	if (lbr_desc->event) {
		perf_event_release_kernel(lbr_desc->event);
		lbr_desc->event = NULL;
		vcpu_to_pmu(vcpu)->event_count--;
	}
}

int intel_pmu_create_guest_lbr_event(struct kvm_vcpu *vcpu)
{
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct perf_event *event;

	/*
	 * The perf_event_attr is constructed in the minimum efficient way:
	 * - set 'pinned = true' to make it task pinned so that if another
	 *   cpu pinned event reclaims LBR, the event->oncpu will be set to -1;
	 * - set '.exclude_host = true' to record guest branches behavior;
	 *
	 * - set '.config = INTEL_FIXED_VLBR_EVENT' to indicates host perf
	 *   schedule the event without a real HW counter but a fake one;
	 *   check is_guest_lbr_event() and __intel_get_event_constraints();
	 *
	 * - set 'sample_type = PERF_SAMPLE_BRANCH_STACK' and
	 *   'branch_sample_type = PERF_SAMPLE_BRANCH_CALL_STACK |
	 *   PERF_SAMPLE_BRANCH_USER' to configure it as a LBR callstack
	 *   event, which helps KVM to save/restore guest LBR records
	 *   during host context switches and reduces quite a lot overhead,
	 *   check branch_user_callstack() and intel_pmu_lbr_sched_task();
	 */
	struct perf_event_attr attr = {
		.type = PERF_TYPE_RAW,
		.size = sizeof(attr),
		.config = INTEL_FIXED_VLBR_EVENT,
		.sample_type = PERF_SAMPLE_BRANCH_STACK,
		.pinned = true,
		.exclude_host = true,
		.branch_sample_type = PERF_SAMPLE_BRANCH_CALL_STACK |
					PERF_SAMPLE_BRANCH_USER,
	};

	if (unlikely(lbr_desc->event)) {
		__set_bit(INTEL_PMC_IDX_FIXED_VLBR, pmu->pmc_in_use);
		return 0;
	}

	event = perf_event_create_kernel_counter(&attr, -1,
						current, NULL, NULL);
	if (IS_ERR(event)) {
		pr_debug_ratelimited("%s: failed %ld\n",
					__func__, PTR_ERR(event));
		return PTR_ERR(event);
	}
	lbr_desc->event = event;
	pmu->event_count++;
	__set_bit(INTEL_PMC_IDX_FIXED_VLBR, pmu->pmc_in_use);
	return 0;
}

/*
 * It's safe to access LBR msrs from guest when they have not
 * been passthrough since the host would help restore or reset
 * the LBR msrs records when the guest LBR event is scheduled in.
 */
static bool emulated_pmu_handle_lbr_msrs_access(struct kvm_vcpu *vcpu,
						struct msr_data *msr_info, bool read)
{
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);
	u32 index = msr_info->index;

	if (!intel_pmu_is_valid_lbr_msr(vcpu, msr_info->index))
		return false;

	if (!lbr_desc->event && intel_pmu_create_guest_lbr_event(vcpu) < 0)
		goto dummy;

	/*
	 * Disable irq to ensure the LBR feature doesn't get reclaimed by the
	 * host at the time the value is read from the msr, and this avoids the
	 * host LBR value to be leaked to the guest. If LBR has been reclaimed,
	 * return 0 on guest reads.
	 */
	local_irq_disable();
	if (lbr_desc->event->state == PERF_EVENT_STATE_ACTIVE) {
		if (read)
			rdmsrl(index, msr_info->data);
		else
			wrmsrl(index, msr_info->data);
		__set_bit(INTEL_PMC_IDX_FIXED_VLBR, vcpu_to_pmu(vcpu)->pmc_in_use);
		local_irq_enable();
		return true;
	}
	clear_bit(INTEL_PMC_IDX_FIXED_VLBR, vcpu_to_pmu(vcpu)->pmc_in_use);
	local_irq_enable();

dummy:
	if (read)
		msr_info->data = 0;
	return true;
}

static bool intel_pmu_handle_extra_msrs_access(struct kvm_vcpu *vcpu,
				     struct msr_data *msr_info, bool read)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	int i;

	for (i = 0; i < kvm_pmu_cap.num_extra_msrs; i++)
		if (kvm_pmu_cap.extra_msrs[i] == msr_info->index) {
			if (read)
				msr_info->data = pmu->extra_msrs[i];
			else
				pmu->extra_msrs[i] = msr_info->data;
			return true;
		}

	return false;
}

static bool mediated_pmu_handle_lbr_msrs_access(struct kvm_vcpu *vcpu,
						struct msr_data *msr_info, bool read)
{
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);
	struct x86_pmu_lbr *records = vcpu_to_lbr_records(vcpu);
	struct lbr_entry *entry = lbr_desc->state->lbr.entries;
	u32 index = msr_info->index;

	if (index == MSR_ARCH_LBR_CTL) {
		if (read) {
			msr_info->data = vmcs_read64(GUEST_IA32_LBR_CTL);
		} else {
			if (msr_info->data & vcpu_to_pmu(vcpu)->arch_lbr_ctrl_rsvd)
				return false;

			if ((vmcs_read64(GUEST_IA32_LBR_CTL) ^ msr_info->data) &
			    ARCH_LBR_CTL_LBREN) {
				if (msr_info->data & ARCH_LBR_CTL_LBREN)
					vmx_enable_lbr_msrs_passthrough(vcpu);
				else
					vmx_disable_lbr_msrs_passthrough(vcpu);
			}

			vmcs_write64(GUEST_IA32_LBR_CTL, msr_info->data);
		}
	} else if (index == MSR_ARCH_LBR_DEPTH) {
		/*
		 * KVM advertises only the host's LBR depth as a supported depth, i.e.
		 * disallows using arch LBRs with a different depth than the host.
		 * Don't bother checking guest CPUID to see if the requested depth is
		 * allowed, as the current depth is the only allowed depth as far as
		 * KVM is concerned.
		 */
		if (read) {
			msr_info->data = records->nr;
		} else {
			if (msr_info->data != records->nr)
				return false;

			/* Write to LBR_DEPTH, reset all LBR entries to 0. */
			memset(entry, 0, lbr_desc->records.nr *
			       sizeof(struct lbr_entry));
		}
	} else if (index >= records->from && index < records->from + records->nr) {
		entry += index - records->from;
		if (read)
			msr_info->data = entry->from;
		else
			entry->from = msr_info->data;
	} else if (index >= records->to && index < records->to + records->nr) {
		entry += index - records->to;
		if (read)
			msr_info->data = entry->to;
		else
			entry->to = msr_info->data;
	} else if (records->info && index >= records->info &&
		   index < records->info + records->nr) {
		entry += index - records->info;
		if (read)
			msr_info->data = entry->info;
		else
			entry->info = msr_info->data;
	} else
		return false;

	return true;
}

static bool intel_pmu_handle_lbr_msrs_access(struct kvm_vcpu *vcpu,
					     struct msr_data *msr_info, bool read)
{
	if (kvm_mediated_pmu_enabled(vcpu))
		return mediated_pmu_handle_lbr_msrs_access(vcpu, msr_info, read);
	else
		return emulated_pmu_handle_lbr_msrs_access(vcpu, msr_info, read);
}

static int intel_pmu_get_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc;
	u32 msr = msr_info->index;

	switch (msr) {
	case MSR_CORE_PERF_GLOBAL_CTRL:
		if (kvm_mediated_pmu_enabled(vcpu))
			pmu->global_ctrl = vmcs_read64(GUEST_IA32_PERF_GLOBAL_CTRL);
		msr_info->data = pmu->global_ctrl;
		break;
	case MSR_CORE_PERF_FIXED_CTR_CTRL:
		msr_info->data = pmu->fixed_ctr_ctrl;
		break;
	case MSR_CORE_PERF_GLOBAL_STATUS_SET:
		/* Write only register. */
		msr_info->data = 0;
		break;
	case MSR_CORE_PERF_GLOBAL_INUSE:
		msr_info->data = pmu->global_inuse;
		break;
	case MSR_PERF_METRICS:
		msr_info->data = pmu->perf_metrics;
		break;
	case MSR_IA32_PEBS_ENABLE:
		msr_info->data = pmu->pebs_enable;
		break;
	case MSR_IA32_DS_AREA:
		msr_info->data = pmu->ds_area;
		break;
	case MSR_PEBS_DATA_CFG:
		msr_info->data = pmu->pebs_data_cfg;
		break;
	case MSR_IA32_PEBS_BASE:
		msr_info->data = pmu->arch_pebs_base;
		break;
	case MSR_IA32_PEBS_INDEX:
		msr_info->data = pmu->arch_pebs_index;
		break;
	case MSR_IA32_RTIT_TRIGGER0_CFG ... MSR_IA32_RTIT_TRIGGER6_CFG:
		u32 idx = msr - MSR_IA32_RTIT_TRIGGER0_CFG;
		msr_info->data = to_vmx(vcpu)->pt_desc.guest.pt.trigger[idx];
		break;
	default:
		if ((pmc = get_gp_pmc(pmu, msr, MSR_IA32_PERFCTR0)) ||
		    (pmc = get_gp_pmc(pmu, msr, MSR_IA32_PMC0)) ||
		    (pmc = get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CTR))) {
			u64 val = pmc_read_counter(pmc);
			msr_info->data =
				val & pmu->counter_bitmask[KVM_PMC_GP];
			break;
		} else if ((pmc = get_fixed_pmc(pmu, msr, MSR_CORE_PERF_FIXED_CTR0)) ||
			   (pmc = get_fixed_pmc(pmu, msr, MSR_IA32_PMC_V6_FX0_CTR))) {
			u64 val = pmc_read_counter(pmc);
			msr_info->data =
				val & pmu->counter_bitmask[KVM_PMC_FIXED];
			break;
		} else if ((pmc = get_gp_pmc(pmu, msr, MSR_P6_EVNTSEL0)) ||
			   (pmc = get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CFG_A))) {
			msr_info->data = pmc->eventsel;
			break;
		} else if ((pmc = get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CFG_C)) ||
			   (pmc = get_fixed_pmc(pmu, msr, MSR_IA32_PMC_V6_FX0_CFG_C))) {
			msr_info->data = pmc->arch_pebs_cfg_c;
			break;
		} else if (intel_pmu_handle_lbr_msrs_access(vcpu, msr_info, true)) {
			break;
		} else if (intel_pmu_handle_extra_msrs_access(vcpu, msr_info, true)) {
			break;
		}

		return 1;
	}

	return 0;
}

static int _intel_pmu_rtit_trigger_check(struct kvm_vcpu *vcpu,
					 unsigned long input,
					 unsigned long action,
					 unsigned long old_input)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	u32 *pt_caps = to_vmx(vcpu)->pt_desc.caps;

	if (test_bit(3, &input) || test_bit(4, &input) || (input > 0x43)) {
		pr_warn("Program reserved input encoding.");
		return 1;
	}

	if (input < 0x28 && !gp_ctr_is_supported(pmu, input & GENMASK(2, 0))) {
		pr_warn("Program unsupported GP counter");
		return 1;
	}

	if (!intel_pt_validate_cap(pt_caps, PT_CAP_dr_match) &&
	    test_bit(6, &input)) {
		pr_warn("DR Match is not supported.");
		return 1;
	}

	if (!intel_pt_validate_cap(pt_caps, PT_CAP_trigger_attribution)
	    && (action & BIT(14))) {
		pr_warn("Trigger attribution is not supported.");
		return 1;
	}

	if (!intel_pt_validate_cap(pt_caps, PT_CAP_pause_resume)
	    && (action & (BIT(12) | BIT(13)))) {
		pr_warn("Pause/Resume is not supported.");
		return 1;
	}

	if (action & BIT(15)) {
		if ((input != old_input)) {
			pr_warn("Config input before setting EN bit.");
			return 1;
		}

		if (input < 0x28) {
			struct kvm_pmc *pmc;

			pmc = get_gp_pmc_from_idx(pmu, input & GENMASK(2, 0));
			if (pmc && !(pmc->eventsel & ARCH_PERFMON_EVENTSEL_EN_PT_LOG)) {
				pr_warn("EVTSELx.EN_PT_LOG not set.");
				return 1;
			}
		} else {
			unsigned long dr7 = kvm_get_dr(vcpu, 7);

			if (!test_bit(32 + input - 0x40, &dr7)) {
				pr_warn("DR7.DRx_PT_LOG not set.");
				return 1;
			}
		}
	}

	return 0;
}

/*
 *  Note: sanity check but doesn't inject #GP, since unsupported encodings in
 *  IA32_RTIT_TRIGGERx_CFG may not generate a fault.
 */
static int intel_pmu_rtit_trigger_check(struct kvm_vcpu *vcpu, u64 data,
					u64 old)
{
	unsigned long input, action, old_input;
	int i;

	if (data & RTIT_TRIGGER_RESERVED) {
		pr_warn("Write reserved bits.");
		return 1;
	}

	for (i = 0; i < 64; i += 16) {
		input = (data >> i) & GENMASK(6, 0);
		action = (data >> i) & GENMASK(15, 12);
		old_input = (old >> i) & GENMASK(6, 0);

		if (_intel_pmu_rtit_trigger_check(vcpu, input, action,
						  old_input))
			return 1;
	}

	return 0;
}

static int intel_pmu_set_msr(struct kvm_vcpu *vcpu, struct msr_data *msr_info)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc;
	u32 msr = msr_info->index;
	u64 data = msr_info->data;
	u64 eventsel_rsvd, diff;
	u64 global_inuse_rsvd;

	switch (msr) {
	case MSR_CORE_PERF_GLOBAL_CTRL:
		if (kvm_mediated_pmu_enabled(vcpu))
			vmcs_write64(GUEST_IA32_PERF_GLOBAL_CTRL,
				     pmu->global_ctrl);
		break;
	case MSR_CORE_PERF_FIXED_CTR_CTRL:
		if (data & pmu->fixed_ctr_ctrl_rsvd)
			return 1;

		if (pmu->fixed_ctr_ctrl != data)
			reprogram_fixed_counters(pmu, data);
		break;
	case MSR_CORE_PERF_GLOBAL_INUSE:
		if (!msr_info->host_initiated)
			return 1; /* RO MSR */

		global_inuse_rsvd = ~(PERF_GLOBAL_INUSE_PMI_INSUSE |
				      pmu->all_valid_pmc_idx64);
		if (data & global_inuse_rsvd)
			return 1;

		pmu->global_inuse = data;
		break;
	case MSR_CORE_PERF_GLOBAL_STATUS_SET:
		/*
		 * GLOBAL STATUS_SET, sets bits in GLOBAL_STATUS, so the
		 * set of reserved bits are the same.
		 */
		if (data & pmu->global_status_rsvd)
			return 1;

		pmu->global_status_set = data;
		break;
	case MSR_PERF_METRICS:
		pmu->perf_metrics = data;
		break;
	case MSR_IA32_PEBS_ENABLE:
		if (data & pmu->pebs_enable_rsvd)
			return 1;

		if (pmu->pebs_enable != data) {
			diff = pmu->pebs_enable ^ data;
			pmu->pebs_enable = data;
			reprogram_counters(pmu, diff);
		}
		break;
	case MSR_IA32_DS_AREA:
		if (is_noncanonical_msr_address(data, vcpu))
			return 1;

		pmu->ds_area = data;
		break;
	case MSR_PEBS_DATA_CFG:
		if (data & pmu->pebs_data_cfg_rsvd)
			return 1;

		pmu->pebs_data_cfg = data;
		break;
	case MSR_IA32_PEBS_BASE:
		pmu->arch_pebs_base = msr_info->data;
		break;
	case MSR_IA32_PEBS_INDEX:
		if (data & pmu->arch_pebs_index_rsvd)
			return 1;

		pmu->arch_pebs_index = msr_info->data;
		break;
	case MSR_IA32_RTIT_TRIGGER0_CFG ... MSR_IA32_RTIT_TRIGGER6_CFG:
		struct vcpu_vmx *vmx = to_vmx(vcpu);
		int idx = msr - MSR_IA32_RTIT_TRIGGER0_CFG;

		if (!pt_can_write_msr(vmx))
			return 1;
		intel_pmu_rtit_trigger_check(vcpu, data,
			vmx->pt_desc.guest.pt.trigger[idx]);
		vmx->pt_desc.guest.pt.trigger[idx] = data;
		break;
	default:
		if ((pmc = get_gp_pmc(pmu, msr, MSR_IA32_PERFCTR0)) ||
		    (pmc = get_gp_pmc(pmu, msr, MSR_IA32_PMC0)) ||
		    (pmc = get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CTR))) {
			if (((msr & MSR_PMC_FULL_WIDTH_BIT) ||
			      msr >= MSR_IA32_PMC_V6_GP0_CTR) &&
			    (data & ~pmu->counter_bitmask[KVM_PMC_GP]))
				return 1;

			if (!msr_info->host_initiated &&
			    msr < MSR_IA32_PMC_V6_GP0_CTR &&
			    !(msr & MSR_PMC_FULL_WIDTH_BIT))
				data = (s64)(s32)data;
			pmc_write_counter(pmc, data);
			break;
		} else if ((pmc = get_fixed_pmc(pmu, msr, MSR_CORE_PERF_FIXED_CTR0)) ||
			   (pmc = get_fixed_pmc(pmu, msr, MSR_IA32_PMC_V6_FX0_CTR))) {
			pmc_write_counter(pmc, data);
			break;
		} else if ((pmc = get_gp_pmc(pmu, msr, MSR_P6_EVNTSEL0)) ||
			   (pmc = get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CFG_A))) {
			eventsel_rsvd = pmu->eventsel_rsvd;
			if ((pmc->idx == 2) &&
			    (pmu->raw_event_mask & HSW_IN_TX_CHECKPOINTED))
				eventsel_rsvd ^= HSW_IN_TX_CHECKPOINTED;
			if (data & eventsel_rsvd)
				return 1;

			if (data != pmc->eventsel) {
				pmc->eventsel = data;
				kvm_pmu_request_counter_reprogram(pmc);
			}
			break;
		} else if ((pmc = get_gp_pmc(pmu, msr, MSR_IA32_PMC_V6_GP0_CFG_C)) ||
			   (pmc = get_fixed_pmc(pmu, msr, MSR_IA32_PMC_V6_FX0_CFG_C))) {
			if (data & pmu->arch_pebs_cfg_c_rsvd)
				return 1;
			pmc->arch_pebs_cfg_c = msr_info->data;
			break;
		} else if (intel_pmu_handle_lbr_msrs_access(vcpu, msr_info, false)) {
			break;
		} else if (intel_pmu_handle_extra_msrs_access(vcpu, msr_info, false)) {
			break;
		}
		/* Not a known PMU MSR. */
		return 1;
	}

	return 0;
}

/*
 * Map fixed counter events to architectural general purpose event encodings.
 * Perf doesn't provide APIs to allow KVM to directly program a fixed counter,
 * and so KVM instead programs the architectural event to effectively request
 * the fixed counter.  Perf isn't guaranteed to use a fixed counter and may
 * instead program the encoding into a general purpose counter, e.g. if a
 * different perf_event is already utilizing the requested counter, but the end
 * result is the same (ignoring the fact that using a general purpose counter
 * will likely exacerbate counter contention).
 *
 * Forcibly inlined to allow asserting on @index at build time, and there should
 * never be more than one user.
 */
static __always_inline u64 intel_get_fixed_pmc_eventsel(struct kvm_pmu *pmu,
							unsigned int index)
{
	const enum perf_hw_id fixed_pmc_perf_ids[] = {
		[0] = PERF_COUNT_HW_INSTRUCTIONS,
		[1] = PERF_COUNT_HW_CPU_CYCLES,
		[2] = PERF_COUNT_HW_REF_CPU_CYCLES,
		[3] = PERF_COUNT_HW_TOPDOWN_SLOTS,
		[4] = PERF_COUNT_HW_TOPDOWN_BAD_SPEC,
		[5] = PERF_COUNT_HW_TOPDOWN_FE_BOUND,
		[6] = PERF_COUNT_HW_TOPDOWN_RETIRING,
	};
	u64 eventsel;

	BUILD_BUG_ON(ARRAY_SIZE(fixed_pmc_perf_ids) != KVM_MAX_NR_INTEL_FIXED_COUTNERS);
	BUILD_BUG_ON(index >= KVM_MAX_NR_INTEL_FIXED_COUTNERS);

	/*
	 * Yell if perf reports support for a fixed counter but perf doesn't
	 * have a known encoding for the associated general purpose event.
	 */
	eventsel = perf_get_hw_event_config(fixed_pmc_perf_ids[index]);
	WARN_ON_ONCE(!eventsel && fixed_ctr_is_supported(pmu, index));
	return eventsel;
}

/*
 * This API purposely doesn't include the pmu->passthrough check so that it can
 * be used before the VM is created.  Since KVM chooses to support Arch LBR on
 * mediated vPMU only, this API returns true is not enough to justify whether
 * or not Arch LBR is enabled for the guest.
 */
bool guest_can_use_arch_lbr(void)
{
	u32 eax, ebx, ecx, edx, size;

	if (!vmx_lbr_caps.nr ||
	    !cpu_feature_enabled(X86_FEATURE_ARCH_LBR) ||
	    !cpu_has_vmx_arch_lbr())
		return false;

	if (!boot_cpu_has(X86_FEATURE_XSAVES) ||
	    !(kvm_host.xss & XFEATURE_MASK_LBR))
		return false;

	/*
	 * KVM doesn't allow guest to configure different LBR depth from the
	 * maximum host depth, which is enumerated by CPUID.1CH.EAX. The size
	 * of the XSAVE LBR state is enumerated by CPUID.0DH Arch LBR leaf,
	 * and the values are supposed to be matched.
	 */
	size = sizeof(struct arch_lbr_state) +
	       vmx_lbr_caps.nr * sizeof(struct lbr_entry);
	cpuid_count(0xd, XFEATURE_LBR, &eax, &ebx, &ecx, &edx);

	if (!eax || WARN_ON(eax != size))
		return false;

	return true;
}

static inline void intel_update_msr_base(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);

	if (pmu->version < 6) {
		pmu->gp_eventsel_base = MSR_P6_EVNTSEL0;
		pmu->gp_counter_base = fw_writes_is_enabled(vcpu) ?
				MSR_IA32_PMC0 : MSR_IA32_PERFCTR0;
		pmu->fixed_base = MSR_CORE_PERF_FIXED_CTR0;
		pmu->cntr_shift = 1;
	} else {
		pmu->gp_eventsel_base = MSR_IA32_PMC_V6_GP0_CFG_A;
		pmu->gp_counter_base = MSR_IA32_PMC_V6_GP0_CTR;
		pmu->fixed_base = MSR_IA32_PMC_V6_FX0_CTR;
		pmu->cntr_shift = 4;
	}
}

static void __intel_pmu_refresh_lbr(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);
	u64 perf_capabilities;

	memset(&lbr_desc->records, 0, sizeof(lbr_desc->records));

	pmu->arch_lbr_ctrl_rsvd = ~(0xfull | 0x7f0000ull);

	/*
	 * Legacy LBR is only available in legacy vPMU and Arch LBR is only
	 * available in mediated vPMU
	 */
	perf_capabilities = vcpu_get_perf_capabilities(vcpu);
	if ((perf_capabilities & PERF_CAP_LBR_FMT) &&
	   ((guest_can_use_arch_lbr() && kvm_mediated_pmu_enabled(vcpu)) ||
	   (cpuid_model_is_consistent(vcpu) && !kvm_mediated_pmu_enabled(vcpu))))
		memcpy(&lbr_desc->records, &vmx_lbr_caps, sizeof(vmx_lbr_caps));
	else
		lbr_desc->records.nr = 0;

	/*
	 * The LBR depth is determined by host capability and it won't be
	 * changed by userspace.
	 */
	if (lbr_desc->records.nr && kvm_mediated_pmu_enabled(vcpu) &&
	    !lbr_desc->state) {
		size_t content_size = sizeof(union arch_lbr_xsave_state) +
			lbr_desc->records.nr * sizeof(struct lbr_entry);

		lbr_desc->state = (union arch_lbr_xsave_state *)
				  kzalloc(content_size, GFP_KERNEL);
		if (!lbr_desc->state)
			lbr_desc->records.nr = 0;
		else
			/*
			 * For guest LBR context switch, XRSTORS is called
			 * before first XSAVES is called.  XRSTORS requires
			 * XCOMP_BV[63] to be set.
			 *
			 * We leave XSTATE_BV[15] to zero so that the first
			 * XRSTORS instruction serves the purpose to set the
			 * Arch LBR state component to it's initial configuration:
			 * all MSR to be 0, besides IA32_LBR_DEPTH.
			 *
			 * The subsequent XSAVES updates XCOMP_BV and XSTATE_BV
			 * from RFBM, which set bit 15 to each of them.
			 **/
			lbr_desc->state->header.xcomp_bv = XCOMP_BV_COMPACTED_FORMAT;
	}

	/* Legacy LBR is only enabled on legacy perf-based vPMU. */
	if (lbr_desc->records.nr && !kvm_mediated_pmu_enabled(vcpu))
		bitmap_set(pmu->all_valid_pmc_idx, INTEL_PMC_IDX_FIXED_VLBR, 1);
}

static void __intel_pmu_refresh(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_cpuid_entry2 *entry;
	struct kvm_cpuid_entry2 *entry23_0 = NULL;
	struct kvm_cpuid_entry2 *entry23_1 = NULL;
	struct kvm_cpuid_entry2 *entry23_3 = NULL;
	struct kvm_cpuid_entry2 *entry23_4 = NULL;
	struct kvm_cpuid_entry2 *entry23_5 = NULL;
	union cpuid10_eax eax;
	union cpuid10_edx edx;
	u64 perf_capabilities;
	u64 fixed_bits;
	u64 gp_bits;
	int i;

	/* CPUID 0xa leaf */
	entry = kvm_find_cpuid_entry(vcpu, 0xa);
	if (!entry)
		return;

	eax.full = entry->eax;
	edx.full = entry->edx;

	/* CPUID 0x23 leaf */
	entry23_0 = kvm_find_cpuid_entry_index(vcpu, 0x23, 0x0);
	if (entry23_0) {
		union cpuid35_eax eax;

		eax.full = entry23_0->eax;
		if (eax.split.cntr_subleaf)
			entry23_1 = kvm_find_cpuid_entry_index(vcpu, 0x23,
						ARCH_PERFMON_NUM_COUNTER_LEAF);
		if (eax.split.events_subleaf)
			entry23_3 = kvm_find_cpuid_entry_index(vcpu, 0x23,
						ARCH_PERFMON_ARCH_EVENTS_LEAF);
		if (eax.split.pebs_caps_subleaf)
			entry23_4 = kvm_find_cpuid_entry_index(vcpu, 0x23,
						ARCH_PERFMON_PEBS_CAP_LEAF);
		if (eax.split.pebs_cnts_subleaf)
			entry23_5 = kvm_find_cpuid_entry_index(vcpu, 0x23,
						ARCH_PERFMON_PEBS_COUNTER_LEAF);
	}

	pmu->version = eax.split.version_id;
	if (!pmu->version)
		return;

	/* GP & Fixed counter bit-width */
	eax.split.bit_width = min_t(int, eax.split.bit_width,
				    kvm_pmu_cap.bit_width_gp);
	pmu->counter_bitmask[KVM_PMC_GP] = BIT_ULL(eax.split.bit_width) - 1;
	if (pmu->version > 1) {
		edx.split.bit_width_fixed = min_t(int, edx.split.bit_width_fixed,
						  kvm_pmu_cap.bit_width_fixed);
		pmu->counter_bitmask[KVM_PMC_FIXED] =
					BIT_ULL(edx.split.bit_width_fixed) - 1;
	}

	/* Events bitmap */
	if (entry23_3) {
		pmu->available_event_types = entry23_3->eax & kvm_pmu_cap.events_mask_ext;
	} else {
		eax.split.mask_length = min_t(int, eax.split.mask_length,
					      kvm_pmu_cap.events_mask_len);
		pmu->available_event_types = ~entry->ebx & (BIT_ULL(eax.split.mask_length) - 1);
	}

	/* GP counter bitmap */
	if (entry23_1) {
		pmu->all_valid_pmc_idx64 = entry23_1->eax & kvm_pmu_cap.cntr_mask64;
	} else {
		pmu->all_valid_pmc_idx64 = (BIT_ULL(eax.split.num_counters) - 1) &
					   kvm_pmu_cap.cntr_mask64;
	}
	pmu->nr_arch_gp_counters = hweight64(pmu->all_valid_pmc_idx64);

	if (kvm_pmu_has_perf_global_ctrl(pmu)) {
		/*
		 * At RESET, Intel CPUs set all enable bits for general purpose counters
		 * in IA32_PERF_GLOBAL_CTRL. Emulate this behavior.
		 */
		pmu->global_ctrl = pmu->all_valid_pmc_idx64;
		pmu->global_ctrl_rsvd = ~pmu->global_ctrl;

		for_each_set_bit(i, kvm_pmu_cap.fixed_cntr_mask, KVM_MAX_NR_INTEL_FIXED_COUTNERS) {
			if (entry23_1) {
				if (!(entry23_1->ebx & BIT_ULL(i)))
					continue;
			} else if (!(entry->ecx & BIT_ULL(i) || edx.split.num_counters_fixed > i))
				/* FxCtr[i]_is_supported := CPUID.0xA.ECX[i] || EDX[4:0] > i */
				continue;

			set_bit(INTEL_PMC_IDX_FIXED + i, pmu->all_valid_pmc_idx);
			pmu->fixed_ctr_ctrl_rsvd &=
				 ~intel_fixed_bits_by_idx(i, INTEL_FIXED_BASIC_BITS_MASK);
			pmu->global_ctrl_rsvd &= ~BIT_ULL(INTEL_PMC_IDX_FIXED + i);
		}

		edx.split.bit_width_fixed = min_t(int, edx.split.bit_width_fixed,
						  kvm_pmu_cap.bit_width_fixed);
		pmu->counter_bitmask[KVM_PMC_FIXED] = BIT_ULL(edx.split.bit_width_fixed) - 1;

		/*
		 * GLOBAL_STATUS and GLOBAL_OVF_CONTROL (a.k.a. GLOBAL_STATUS_RESET)
		 * share reserved bit definitions.  The kernel just happens to use
		 * OVF_CTRL for the names.
		 */
		pmu->global_status_rsvd = pmu->global_ctrl_rsvd &
					  ~(MSR_CORE_PERF_GLOBAL_OVF_CTRL_OVF_BUF |
					    MSR_CORE_PERF_GLOBAL_OVF_CTRL_COND_CHGD);

		if (pmu->version >= 5)
			pmu->global_status_rsvd &=
					~(MSR_CORE_PERF_GLOBAL_STATUS_LBR_FREEZE |
					  MSR_CORE_PERF_GLOBAL_STATUS_CTR_FREEZE |
					  MSR_CORE_PERF_GLOBAL_STATUS_ASCI |
					  MSR_CORE_PERF_GLOBAL_OVF_CTRL_OVF_UNCORE);
	}

	if (guest_cpu_cap_has(vcpu, X86_FEATURE_INTEL_PT)) {
		pmu->global_status_rsvd &= ~MSR_CORE_PERF_GLOBAL_OVF_CTRL_TRACE_TOPA_PMI;

		entry = kvm_find_cpuid_entry_index(vcpu, 0x14, 0);
		if (entry && entry->ebx & BIT(9))
			pmu->eventsel_rsvd &= ~ARCH_PERFMON_EVENTSEL_EN_PT_LOG;
	}

	if (guest_cpu_cap_has(vcpu, X86_FEATURE_HLE) ||
	    guest_cpu_cap_has(vcpu, X86_FEATURE_RTM)) {
		pmu->eventsel_rsvd ^= HSW_IN_TX;
		pmu->raw_event_mask |= (HSW_IN_TX|HSW_IN_TX_CHECKPOINTED);
	}

	if (pmu->version >= 6) {
		union cpuid35_ebx ebx;

		ebx.full = entry23_0->ebx;
		if (ebx.split.umask2)
			pmu->eventsel_rsvd &= ~ARCH_PERFMON_EVENTSEL_UMASK2;
		if (ebx.split.eq)
			pmu->eventsel_rsvd &= ~ARCH_PERFMON_EVENTSEL_EQ;
		if (ebx.split.rdpmc_user_disable)
			pmu->eventsel_rsvd &= ~ARCH_PERFMON_EVENTSEL_RDPMC_USER_DISABLE;

		pmu->eventsel_rsvd |= ARCH_PERFMON_EVENTSEL_PIN_CONTROL;
	}

	entry = kvm_find_cpuid_entry_index(vcpu, 0x1c, 0);
	if (entry && (entry->ecx & GENMASK(19, 16)))
		pmu->eventsel_rsvd &= ~ARCH_PERFMON_EVENTSEL_BR_CNTR;

	__intel_pmu_refresh_lbr(vcpu);

	fixed_bits = fixed_ctrs_bitmap(pmu);
	gp_bits = gp_ctrs_bitmap(pmu);
	perf_capabilities = vcpu_get_perf_capabilities(vcpu);
	if (perf_capabilities & PERF_CAP_PEBS_FORMAT) {
		if (perf_capabilities & PERF_CAP_PEBS_BASELINE) {
			pmu->pebs_enable_rsvd = pmu->global_ctrl_rsvd;
			pmu->eventsel_rsvd &= ~ICL_EVENTSEL_ADAPTIVE;

			for_each_set_bit(i, (unsigned long*)&fixed_bits,
					 KVM_MAX_NR_INTEL_FIXED_COUTNERS) {
				pmu->fixed_ctr_ctrl_rsvd &=
					~intel_fixed_bits_by_idx(i, ICL_FIXED_0_ADAPTIVE);
			}
			pmu->pebs_data_cfg_rsvd = ~0xff00000full;
		} else {
			pmu->pebs_enable_rsvd = ~gp_bits;
		}
	}

	pmu->perf_metrics = 0;
	if (perf_capabilities & PERF_CAP_PERF_METRICS) {
		pmu->global_ctrl_rsvd &= ~GLOBAL_CTRL_EN_PERF_METRICS;
		pmu->global_status_rsvd &= ~GLOBAL_STATUS_PERF_METRICS_OVF;
	}

	intel_update_msr_base(vcpu);

	pmu->arch_pebs = kvm_pmu_cap.arch_pebs && entry23_4 && entry23_5;
	pmu->arch_pebs_base = 0;
	pmu->arch_pebs_index = 0;
	pmu->arch_pebs_index_rsvd = GENMASK_ULL(3, 0) | GENMASK_ULL(30, 27) |
				    GENMASK_ULL(35, 33) | GENMASK_ULL(63, 59);
	pmu->arch_pebs_cfg_c_rsvd = GENMASK_ULL(34, 32) | BIT_ULL(39) |
				    GENMASK_ULL(48, 42);
}

static void intel_pmu_update_msr_intercepts(struct kvm_vcpu *vcpu)
{
	bool intercept = !kvm_mediated_pmu_enabled(vcpu);
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	u64 fixed_bits = fixed_ctrs_bitmap(pmu);
	u64 gp_bits = gp_ctrs_bitmap(pmu);
	u64 unsupported_fixed_bits;
	u64 unsupported_gp_bits;
	int i;

	for_each_set_bit(i, (unsigned long*)&gp_bits, KVM_MAX_NR_INTEL_GP_COUNTERS) {
		vmx_set_intercept_for_msr(vcpu, MSR_IA32_PERFCTR0 + i,
					  MSR_TYPE_RW, intercept);
		vmx_set_intercept_for_msr(vcpu, MSR_IA32_PMC0 + i, MSR_TYPE_RW,
					  intercept || !fw_writes_is_enabled(vcpu));
		vmx_set_intercept_for_msr(vcpu, pmu_v6_msr(MSR_IA32_PMC_V6_GP0_CTR, i),
					  MSR_TYPE_RW,
					  intercept || !fw_writes_is_enabled(vcpu) ||
					  pmu->version < 6);
		vmx_set_intercept_for_msr(vcpu, pmu_v6_msr(MSR_IA32_PMC_V6_GP0_CFG_C, i),
					  MSR_TYPE_RW,
					  intercept || !pmu->arch_pebs);
	}

	unsupported_gp_bits = kvm_pmu_cap.cntr_mask64 & ~gp_bits;
	for_each_set_bit(i, (unsigned long*)&unsupported_gp_bits, KVM_MAX_NR_INTEL_GP_COUNTERS) {
		vmx_set_intercept_for_msr(vcpu, MSR_IA32_PERFCTR0 + i,
					  MSR_TYPE_RW, true);
		vmx_set_intercept_for_msr(vcpu, MSR_IA32_PMC0 + i,
					  MSR_TYPE_RW, true);
		vmx_set_intercept_for_msr(vcpu, pmu_v6_msr(MSR_IA32_PMC_V6_GP0_CTR, i),
					  MSR_TYPE_RW, true);
		vmx_set_intercept_for_msr(vcpu, pmu_v6_msr(MSR_IA32_PMC_V6_GP0_CFG_C, i),
					  MSR_TYPE_RW, true);
	}

	for_each_set_bit(i, (unsigned long*)&fixed_bits, KVM_MAX_NR_INTEL_FIXED_COUTNERS) {
		vmx_set_intercept_for_msr(vcpu, MSR_CORE_PERF_FIXED_CTR0 + i,
					  MSR_TYPE_RW, intercept);
		vmx_set_intercept_for_msr(vcpu, pmu_v6_msr(MSR_IA32_PMC_V6_FX0_CTR, i),
					  MSR_TYPE_RW,
					  intercept || pmu->version < 6);
		vmx_set_intercept_for_msr(vcpu, pmu_v6_msr(MSR_IA32_PMC_V6_FX0_CFG_C, i),
					  MSR_TYPE_RW,
					  intercept || !pmu->arch_pebs);
	}

	unsupported_fixed_bits = kvm_pmu_cap.fixed_cntr_mask64 & ~fixed_bits;
	for_each_set_bit(i, (unsigned long*)&unsupported_fixed_bits, KVM_MAX_NR_INTEL_FIXED_COUTNERS) {
		vmx_set_intercept_for_msr(vcpu, MSR_CORE_PERF_FIXED_CTR0 + i,
					  MSR_TYPE_RW, true);
		vmx_set_intercept_for_msr(vcpu, pmu_v6_msr(MSR_IA32_PMC_V6_FX0_CTR, i),
					  MSR_TYPE_RW, true);
		vmx_set_intercept_for_msr(vcpu, pmu_v6_msr(MSR_IA32_PMC_V6_FX0_CFG_C, i),
					  MSR_TYPE_RW, true);
	}

	if (kvm_mediated_pmu_enabled(vcpu) && kvm_pmu_has_perf_global_ctrl(pmu) &&
	    vcpu_has_perf_metrics(vcpu) == kvm_host_has_perf_metrics() &&
	    gp_bits == kvm_pmu_cap.cntr_mask64 &&
	    fixed_bits == kvm_pmu_cap.fixed_cntr_mask64)
		intercept = false;
	else
		intercept = true;

	vmx_set_intercept_for_msr(vcpu, MSR_CORE_PERF_GLOBAL_STATUS,
				  MSR_TYPE_RW, intercept);
	vmx_set_intercept_for_msr(vcpu, MSR_CORE_PERF_GLOBAL_CTRL,
				  MSR_TYPE_RW, intercept);
	vmx_set_intercept_for_msr(vcpu, MSR_CORE_PERF_GLOBAL_OVF_CTRL,
				  MSR_TYPE_RW, intercept);
	vmx_set_intercept_for_msr(vcpu, MSR_CORE_PERF_GLOBAL_STATUS_SET,
				  MSR_TYPE_RW, intercept);
	vmx_set_intercept_for_msr(vcpu, MSR_CORE_PERF_GLOBAL_INUSE,
				  MSR_TYPE_RW, intercept);

	/* legacy PEBS */
	vmx_set_intercept_for_msr(vcpu, MSR_IA32_DS_AREA, MSR_TYPE_RW,
				  intercept || !legacy_pebs_is_enabled(vcpu));
	vmx_set_intercept_for_msr(vcpu, MSR_PEBS_DATA_CFG, MSR_TYPE_RW,
				  intercept || !pebs_baseline_is_enabled(vcpu));
	vmx_set_intercept_for_msr(vcpu, MSR_IA32_PEBS_ENABLE, MSR_TYPE_RW,
				  intercept || !pebs_baseline_is_enabled(vcpu));

	/* arch PEBS */
	vmx_set_intercept_for_msr(vcpu, MSR_IA32_PEBS_BASE, MSR_TYPE_RW,
				  intercept || !pmu->arch_pebs);
	vmx_set_intercept_for_msr(vcpu, MSR_IA32_PEBS_INDEX, MSR_TYPE_RW,
				  intercept || !pmu->arch_pebs);

	/* All extra MSRs are model specific */
	intercept = intercept || !cpuid_model_is_consistent(vcpu);
	for (i = 0; i < kvm_pmu_cap.num_extra_msrs; i++)
		vmx_set_intercept_for_msr(vcpu, kvm_pmu_cap.extra_msrs[i],
					  MSR_TYPE_RW, intercept);

	vmx_set_intercept_for_msr(vcpu, MSR_PERF_METRICS, MSR_TYPE_RW,
				  !vcpu_has_perf_metrics(vcpu));
}

static void intel_pmu_refresh(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct vcpu_vmx *vmx = to_vmx(vcpu);
	bool mediated;
	bool arch_lbr;

	/*
	 * In legacy (non-mediated) vPMU, setting passthrough of LBR MSRs is
	 * done only in the VM-Entry loop, while in mediated vPMU, LBR MSRs
	 * is passthrough after LBR_CTL.LBREn is set by the guest.
	 *
	 * PMU refresh is disallowed after the vCPU has run, i.e. this code
	 * should never be reached while KVM is passing through MSRs.
	 *
	 */
	if (KVM_BUG_ON(vcpu_to_lbr_desc(vcpu)->msr_passthrough, vcpu->kvm))
		return;

	__intel_pmu_refresh(vcpu);

	exec_controls_changebit(vmx, CPU_BASED_RDPMC_EXITING,
				!kvm_rdpmc_in_guest(vcpu));

	/*
	 * FIXME: Drop the MSR bitmap check if/when kvm_pmu_init() no longer
	 *        calls kvm_pmu_refresh(), i.e. when KVM refreshes the PMU only
	 *        after vmcs01 is allocated.
	 */
	if (vmx->vmcs01.msr_bitmap)
		intel_pmu_update_msr_intercepts(vcpu);

	mediated = kvm_mediated_pmu_enabled(vcpu);
	if (cpu_has_load_perf_global_ctrl()) {
		vm_entry_controls_changebit(vmx,
			VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL, mediated);
		/*
		 * Initialize guest PERF_GLOBAL_CTRL to reset value as SDM rules.
		 *
		 * Note: GUEST_IA32_PERF_GLOBAL_CTRL must be initialized to
		 * gp_ctrs_bitmap(pmu) instead of pmu->global_ctrl since
		 * pmu->global_ctrl is only be initialized when guest pmu->version > 1.
		 * Otherwise if pmu->version is 1, pmu->global_ctrl is 0 and guest
		 * counters are never really enabled.
		 */
		if (mediated)
			vmcs_write64(GUEST_IA32_PERF_GLOBAL_CTRL, gp_ctrs_bitmap(pmu));
	}

	if (cpu_has_save_perf_global_ctrl())
		vm_exit_controls_changebit(vmx,
			VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL |
			VM_EXIT_SAVE_IA32_PERF_GLOBAL_CTRL, mediated);

	arch_lbr = mediated && guest_cpu_cap_has(vcpu, X86_FEATURE_ARCH_LBR);
	vm_exit_controls_changebit(vmx, VM_EXIT_CLEAR_IA32_LBR_CTL, arch_lbr);
	vm_entry_controls_changebit(vmx, VM_ENTRY_LOAD_IA32_LBR_CTL, arch_lbr);
}

static void intel_pmu_init(struct kvm_vcpu *vcpu)
{
	int i;
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);

	for (i = 0; i < KVM_MAX_NR_INTEL_GP_COUNTERS; i++) {
		pmu->gp_counters[i].type = KVM_PMC_GP;
		pmu->gp_counters[i].vcpu = vcpu;
		pmu->gp_counters[i].idx = i;
		pmu->gp_counters[i].current_config = 0;
	}

	for (i = 0; i < KVM_MAX_NR_INTEL_FIXED_COUTNERS; i++) {
		pmu->fixed_counters[i].type = KVM_PMC_FIXED;
		pmu->fixed_counters[i].vcpu = vcpu;
		pmu->fixed_counters[i].idx = i + KVM_FIXED_PMC_BASE_IDX;
		pmu->fixed_counters[i].current_config = 0;
		pmu->fixed_counters[i].eventsel = intel_get_fixed_pmc_eventsel(pmu, i);
	}

	lbr_desc->records.nr = 0;
	lbr_desc->event = NULL;
	lbr_desc->state = NULL;
	lbr_desc->msr_passthrough = false;
}

static void intel_pmu_reset(struct kvm_vcpu *vcpu)
{
	intel_pmu_release_guest_lbr_event(vcpu);
}

static void intel_pmu_destroy(struct kvm_vcpu *vcpu)
{
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);

	kfree(lbr_desc->state);
	lbr_desc->state = NULL;
}

/*
 * Emulate legacy and streamlined Freeze_LBR_On_PMI behavior.
 * In either case, guest needs to re-enable LBR to resume branches recording.
 */
static void intel_pmu_freeze_lbr_on_pmi(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	u64 data = vmcs_read64(GUEST_IA32_DEBUGCTL);
	u8 version = vcpu_to_pmu(vcpu)->version;

	if (!(data & DEBUGCTLMSR_FREEZE_LBRS_ON_PMI))
		return;

	/* Legacy Freeze_LBR_on_PMI is supported */
	if (version > 1 && version < 4) {
		if (data & DEBUGCTLMSR_LBR) {
			data &= ~DEBUGCTLMSR_LBR;
			vmcs_write64(GUEST_IA32_DEBUGCTL, data);
		}
	} else if (vcpu_to_lbr_desc(vcpu)->msr_passthrough)
		/*
		 * Arch LBR is supported in mediated vPMU only, this implies:
		 * - It's a mediated vPMU.
		 * - Streamlined Freeze_LBR_on_PMI is supported.
		 * - Guest LBR is enabled.
		 *
		 * pmu->global_status will be restored to guest context before
		 * VM Entry to disable guest LBR recording before it's cleared
		 * by the guest OS.
		 */
		pmu->global_status |= MSR_CORE_PERF_GLOBAL_STATUS_LBR_FREEZE;
}

static void intel_pmu_deliver_pmi(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);

	if (kvm_mediated_pmu_enabled(vcpu)) {
		/* PT system mode should still be handled by host */
		if (vmx_pt_mode_is_system() && test_and_clear_bit(
				GLOBAL_STATUS_TRACE_TOPAPMI_BIT,
				(unsigned long *)&pmu->global_status))
			intel_pt_interrupt();
	}

	if (!intel_pmu_lbr_is_enabled(vcpu))
		return;

	intel_pmu_freeze_lbr_on_pmi(vcpu);
}

static void vmx_update_intercept_for_lbr_msrs(struct kvm_vcpu *vcpu, bool set)
{
	struct x86_pmu_lbr *lbr = vcpu_to_lbr_records(vcpu);
	int i;

	for (i = 0; i < lbr->nr; i++) {
		vmx_set_intercept_for_msr(vcpu, lbr->from + i, MSR_TYPE_RW, set);
		vmx_set_intercept_for_msr(vcpu, lbr->to + i, MSR_TYPE_RW, set);
		if (lbr->info)
			vmx_set_intercept_for_msr(vcpu, lbr->info + i, MSR_TYPE_RW, set);
	}

	if (!cpu_feature_enabled(X86_FEATURE_ARCH_LBR)) {
		vmx_set_intercept_for_msr(vcpu, MSR_LBR_SELECT, MSR_TYPE_RW, set);
		vmx_set_intercept_for_msr(vcpu, MSR_LBR_TOS, MSR_TYPE_RW, set);
	}
}

static inline void vmx_disable_lbr_msrs_passthrough(struct kvm_vcpu *vcpu)
{
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);

	if (!lbr_desc->msr_passthrough)
		return;

	vmx_update_intercept_for_lbr_msrs(vcpu, true);
	lbr_desc->msr_passthrough = false;
}

static inline void vmx_enable_lbr_msrs_passthrough(struct kvm_vcpu *vcpu)
{
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);

	if (lbr_desc->msr_passthrough)
		return;

	vmx_update_intercept_for_lbr_msrs(vcpu, false);
	lbr_desc->msr_passthrough = true;
}

/*
 * Higher priority host perf events (e.g. cpu pinned) could reclaim the
 * pmu resources (e.g. LBR) that were assigned to the guest. This is
 * usually done via ipi calls (more details in perf_install_in_context).
 *
 * Before entering the non-root mode (with irq disabled here), double
 * confirm that the pmu features enabled to the guest are not reclaimed
 * by higher priority host events. Otherwise, disallow vcpu's access to
 * the reclaimed features.
 */
void vmx_passthrough_lbr_msrs(struct kvm_vcpu *vcpu)
{
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);

	if (guest_cpu_cap_has(vcpu, X86_FEATURE_ARCH_LBR))
		return;

	if (!lbr_desc->event) {
		vmx_disable_lbr_msrs_passthrough(vcpu);
		if (vmcs_read64(GUEST_IA32_DEBUGCTL) & DEBUGCTLMSR_LBR)
			goto warn;
		if (test_bit(INTEL_PMC_IDX_FIXED_VLBR, pmu->pmc_in_use))
			goto warn;
		return;
	}

	if (lbr_desc->event->state < PERF_EVENT_STATE_ACTIVE) {
		vmx_disable_lbr_msrs_passthrough(vcpu);
		__clear_bit(INTEL_PMC_IDX_FIXED_VLBR, pmu->pmc_in_use);
		goto warn;
	} else
		vmx_enable_lbr_msrs_passthrough(vcpu);

	return;

warn:
	pr_warn_ratelimited("vcpu-%d: fail to passthrough LBR.\n", vcpu->vcpu_id);
}

static void intel_pmu_cleanup(struct kvm_vcpu *vcpu)
{
	if (!(vmcs_read64(GUEST_IA32_DEBUGCTL) & DEBUGCTLMSR_LBR))
		intel_pmu_release_guest_lbr_event(vcpu);
}

void intel_pmu_cross_mapped_check(struct kvm_pmu *pmu)
{
	struct kvm_pmc *pmc = NULL;
	int bit, hw_idx;

	kvm_for_each_pmc(pmu, pmc, bit, (unsigned long *)&pmu->global_ctrl) {
		if (!pmc_speculative_in_use(pmc) ||
		    !pmc_is_globally_enabled(pmc) || !pmc->perf_event)
			continue;

		/*
		 * A negative index indicates the event isn't mapped to a
		 * physical counter in the host, e.g. due to contention.
		 */
		hw_idx = pmc->perf_event->hw.idx;
		if (hw_idx != pmc->idx && hw_idx > -1)
			pmu->host_cross_mapped_mask |= BIT_ULL(hw_idx);
	}
}

static void pt_load_msr(struct pt_desc *pt_desc, bool is_host)
{
	union intel_pt_xsave_state *state;
	u32 i;

	state = is_host ? &pt_desc->host : &pt_desc->guest;

	wrmsrl(MSR_IA32_RTIT_STATUS, state->pt.status);
	wrmsrl(MSR_IA32_RTIT_OUTPUT_BASE, state->pt.output_base);
	wrmsrl(MSR_IA32_RTIT_OUTPUT_MASK, state->pt.output_mask);
	wrmsrl(MSR_IA32_RTIT_CR3_MATCH, state->pt.cr3_match);

	for (i = 0; i < pt_desc->num_address_ranges * 2; i++)
		wrmsrl(MSR_IA32_RTIT_ADDR0_A + i, state->pt.addr_ab[i]);

	for (i = 0; i < pt_desc->num_trigger_msrs; i++)
		wrmsrl(MSR_IA32_RTIT_TRIGGER0_CFG + i, state->pt.trigger[i]);
}

static void pt_save_msr(struct pt_desc *pt_desc, bool is_host)
{
	union intel_pt_xsave_state *state;
	u32 i;

	state = is_host ? &pt_desc->host : &pt_desc->guest;

	rdmsrl(MSR_IA32_RTIT_STATUS, state->pt.status);
	rdmsrl(MSR_IA32_RTIT_OUTPUT_BASE, state->pt.output_base);
	rdmsrl(MSR_IA32_RTIT_OUTPUT_MASK, state->pt.output_mask);
	rdmsrl(MSR_IA32_RTIT_CR3_MATCH, state->pt.cr3_match);

	for (i = 0; i < pt_desc->num_address_ranges * 2; i++)
		rdmsrl(MSR_IA32_RTIT_ADDR0_A + i, state->pt.addr_ab[i]);

	for (i = 0; i < pt_desc->num_trigger_msrs; i++)
		rdmsrl(MSR_IA32_RTIT_TRIGGER0_CFG + i, state->pt.trigger[i]);
}

static void intel_pmu_put_guest_pt(struct vcpu_vmx *vmx)
{
	if (!guest_cpu_cap_has(&vmx->vcpu, X86_FEATURE_INTEL_PT))
		return;

	if (vmx->pt_desc.guest_rtit_ctl & RTIT_CTL_TRACEEN) {
		pt_save_msr(&vmx->pt_desc, false);
		pt_load_msr(&vmx->pt_desc, true);
	}

	/*
	 * KVM requires VM_EXIT_CLEAR_IA32_RTIT_CTL to expose PT to the guest,
	 * i.e. RTIT_CTL is always cleared on VM-Exit.  Restore it if necessary.
	 */
	if (vmx->pt_desc.host.pt.ctl)
		wrmsrl(MSR_IA32_RTIT_CTL, vmx->pt_desc.host.pt.ctl);
}

static void intel_pmu_load_guest_pt(struct vcpu_vmx *vmx)
{
	if (!guest_cpu_cap_has(&vmx->vcpu, X86_FEATURE_INTEL_PT))
		return;

	/*
	 * In host/guest mode, "load IA32_RTIT_CTL” VM-entry control is always
	 * on, which requires IA32_RTIT_CTL.TraceEn = 0 at the time of VM entry.
	 *
	 * Writing this bit to 0 to satisfy the VM entry check requirement, and
	 * the actual guest RTIT_CTL value will be loaded by "load IA32_RTIT_CTL”
	 * VM-entry control.
	 *
	 * Additionally, now perf_guest_enter() has been called, all exclude_guest
	 * perf events have been scheduled out, and LVTPC vector has been
	 * configured to the KVM dedicated vector.  Thus the host PMI handler
	 * doesn't have a chance to overwrite TraceEn bit as it possbily does in
	 * the legacy non-mediated vPMU implementation.
	 */
	wrmsrl(MSR_IA32_RTIT_CTL, 0);

	if (vmx->pt_desc.guest_rtit_ctl & RTIT_CTL_TRACEEN) {
		pt_save_msr(&vmx->pt_desc, true);
		pt_load_msr(&vmx->pt_desc, false);
	}
}

static void intel_put_guest_context(struct kvm_vcpu *vcpu)
{
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	struct kvm_pmc *pmc;
	u64 fixed_bits;
	u64 gp_bits;
	u32 msr;
	int i;

	/* Global ctrl register is already saved at VM-exit. */
	rdmsrl(MSR_CORE_PERF_GLOBAL_STATUS, pmu->global_status);
	rdmsrl(MSR_CORE_PERF_GLOBAL_INUSE, pmu->global_inuse);
	/* Clear hardware MSR_CORE_PERF_GLOBAL_STATUS MSR, if non-zero. */
	if (pmu->global_status)
		wrmsrl(MSR_CORE_PERF_GLOBAL_OVF_CTRL, pmu->global_status);

	rdmsrl(MSR_CORE_PERF_FIXED_CTR_CTRL, pmu->fixed_ctr_ctrl);

	/*
	 * Clear hardware FIXED_CTR_CTRL MSR to avoid information leakage and
	 * also avoid these guest fixed counters get accidentially enabled
	 * during host running when host enable global ctrl.
	 */
	if (pmu->fixed_ctr_ctrl)
		wrmsrl(MSR_CORE_PERF_FIXED_CTR_CTRL, 0);

	if (vcpu_has_perf_metrics(vcpu)) {
		/*
		 * PERF_METRICS MSR must be read before clear fixed counter 3
		 * (the below kvm_pmu_put_guest_pmcs()), otherwise clearing
		 * fixed counter 3 would clear PERF_METRICS MSR as well.
		 */
		rdpmcl(INTEL_PMC_FIXED_RDPMC_METRICS, pmu->perf_metrics);
		if (pmu->perf_metrics)
			wrmsrl(MSR_PERF_METRICS, 0);
	}

	kvm_pmu_put_guest_pmcs(vcpu);

	for (i = 0; i < kvm_pmu_cap.num_extra_msrs; i++) {
		rdmsrl(kvm_pmu_cap.extra_msrs[i], pmu->extra_msrs[i]);

		if (pmu->extra_msrs[i])
			wrmsrl(kvm_pmu_cap.extra_msrs[i], 0);
	}

	if (legacy_pebs_is_enabled(vcpu)) {
		/* Legacy PEBS */
		rdmsrl(MSR_IA32_DS_AREA, pmu->ds_area);
		if (pebs_baseline_is_enabled(vcpu)) {
			rdmsrl(MSR_PEBS_DATA_CFG, pmu->pebs_data_cfg);
			rdmsrl(MSR_IA32_PEBS_ENABLE, pmu->pebs_enable);
		}
	}

	if (pmu->arch_pebs) {
		rdmsrl(MSR_IA32_PEBS_BASE, pmu->arch_pebs_base);
		rdmsrl(MSR_IA32_PEBS_INDEX, pmu->arch_pebs_index);

		gp_bits = gp_ctrs_bitmap(pmu);
		for_each_set_bit(i, (unsigned long*)&gp_bits, KVM_MAX_NR_INTEL_GP_COUNTERS) {
			pmc = &pmu->gp_counters[i];
			msr = pmu_v6_msr(MSR_IA32_PMC_V6_GP0_CFG_C, i);
			rdmsrl(msr, pmc->arch_pebs_cfg_c);
			if (pmc->arch_pebs_cfg_c)
				wrmsrl(msr, 0);
		}

		fixed_bits = fixed_ctrs_bitmap(pmu);
		for_each_set_bit(i, (unsigned long*)&fixed_bits, KVM_MAX_NR_INTEL_FIXED_COUTNERS) {
			pmc = &pmu->fixed_counters[i];
			msr = pmu_v6_msr(MSR_IA32_PMC_V6_FX0_CFG_C, i);
			rdmsrl(msr, pmc->arch_pebs_cfg_c);
			if (pmc->arch_pebs_cfg_c)
				wrmsrl(msr, 0);
		}
	}

	if (lbr_desc->msr_passthrough) {
		xsaves(&vcpu_to_lbr_desc(vcpu)->state->xsave,
		       XFEATURE_MASK_LBR);

		/* Reset LBR records to avoid guest LBRs leak to host. */
		wrmsrl(MSR_ARCH_LBR_DEPTH, vcpu_to_lbr_records(vcpu)->nr);
	}

	intel_pmu_put_guest_pt(to_vmx(vcpu));
}

static void intel_load_guest_context(struct kvm_vcpu *vcpu)
{
	struct lbr_desc *lbr_desc = vcpu_to_lbr_desc(vcpu);
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	u64 global_status, toggle;
	struct kvm_pmc *pmc;
	u64 fixed_bits;
	u64 gp_bits;
	int i;

	/* Clear host global_ctrl MSR if non-zero. */
	wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, 0);

	rdmsrl(MSR_CORE_PERF_GLOBAL_STATUS, global_status);
	toggle = pmu->global_status ^ global_status;
	if (global_status & toggle)
		wrmsrl(MSR_CORE_PERF_GLOBAL_OVF_CTRL, global_status & toggle);
	if (pmu->global_status & toggle)
		wrmsrl(MSR_CORE_PERF_GLOBAL_STATUS_SET, pmu->global_status & toggle);

	wrmsrl(MSR_CORE_PERF_FIXED_CTR_CTRL, pmu->fixed_ctr_ctrl);

	kvm_pmu_load_guest_pmcs(vcpu);

	/* PERF_METRICS MSR must be restored after fixed counter 3. */
	if (vcpu_has_perf_metrics(vcpu))
		wrmsrl(MSR_PERF_METRICS, pmu->perf_metrics);
	else if (kvm_host_has_perf_metrics())
		wrmsrl(MSR_PERF_METRICS, 0);

	for (i = 0; i < kvm_pmu_cap.num_extra_msrs; i++)
		wrmsrl(kvm_pmu_cap.extra_msrs[i], pmu->extra_msrs[i]);

	if (legacy_pebs_is_enabled(vcpu)) {
		/* Legacy PEBS */
		wrmsrl(MSR_IA32_DS_AREA, pmu->ds_area);
		if (pebs_baseline_is_enabled(vcpu)) {
			wrmsrl(MSR_PEBS_DATA_CFG, pmu->pebs_data_cfg);
			wrmsrl(MSR_IA32_PEBS_ENABLE, pmu->pebs_enable);
		}
	}

	if (pmu->arch_pebs) {
		wrmsrl(MSR_IA32_PEBS_BASE, pmu->arch_pebs_base);
		wrmsrl(MSR_IA32_PEBS_INDEX, pmu->arch_pebs_index);

		gp_bits = gp_ctrs_bitmap(pmu);
		for_each_set_bit(i, (unsigned long*)&gp_bits, KVM_MAX_NR_INTEL_GP_COUNTERS) {
			pmc = &pmu->gp_counters[i];
			wrmsrl(pmu_v6_msr(MSR_IA32_PMC_V6_GP0_CFG_C, i),
			       pmc->arch_pebs_cfg_c);
		}

		fixed_bits = fixed_ctrs_bitmap(pmu);
		for_each_set_bit(i, (unsigned long*)&fixed_bits, KVM_MAX_NR_INTEL_FIXED_COUTNERS) {
			pmc = &pmu->fixed_counters[i];
			wrmsrl(pmu_v6_msr(MSR_IA32_PMC_V6_FX0_CFG_C, i),
			       pmc->arch_pebs_cfg_c);
		}
	}

	if (lbr_desc->msr_passthrough)
		/*
		 * MSR_ARCH_LBR_CTL is restored here, but later on it will load
		 * again from GUEST_IA32_LBR_CTL through VMCS control
		 * VM_ENTRY_LOAD_IA32_LBR_CTL in VM Entry.
		 */
		xrstors(&vcpu_to_lbr_desc(vcpu)->state->xsave,
			XFEATURE_MASK_LBR);

	intel_pmu_load_guest_pt(to_vmx(vcpu));
}

static bool intel_pmu_context_switch_need_skip(struct kvm_vcpu *vcpu)
{
	union vmx_exit_reason exit_reason = to_vmx(vcpu)->exit_reason;
	struct kvm_pmu *pmu = vcpu_to_pmu(vcpu);
	u32 intr_info = vmx_get_intr_info(vcpu);
	u64 pebs_overflow = pmu->global_status &
			    (GLOBAL_STATUS_BUFFER_OVF |
			     GLOBAL_STATUS_ARCH_PEBS_THRESHOLD);

	/*
	 * If sampling period is set to too small like <=2, it may lead to
	 * the speed of handling PEBS overflow PMI can't catch up with the
	 * speed of PEBS overflow PMI generation. Thus the pending guest PEBS
	 * overflow PMI could be delivered after KVM calls perf_guest_exit() to
	 * switch back PMI to NMI (clearing PMI mask bit). This guest PMI would
	 * be recognized a suspicious host NMI and directly dropped instead of
	 * re-injecting into guest.
	 *
	 * Since no PMI is injected into guest, the PEBS overflow bit in guest
	 * global_status would never be cleared and then it blocks to generate
	 * new PEBS overflow PMI. So it traps a deadlock and no PEBS records can
	 * be captured eventually after the suspicious NMI happens.
	 *
	 * To avoid this issue, don't switch guest/host PMU state if guest PEBS
	 * overflow PMI has been armed but not delivered.
	 */
	if (pebs_overflow &&
	    !(exit_reason.basic == EXIT_REASON_EXTERNAL_INTERRUPT &&
	      is_intr_type_n(intr_info, INTR_TYPE_EXT_INTR, KVM_GUEST_PMI_VECTOR)))
		return true;

	return false;
}

struct kvm_pmu_ops intel_pmu_ops __initdata = {
	.rdpmc_ecx_to_pmc = intel_rdpmc_ecx_to_pmc,
	.msr_idx_to_pmc = intel_msr_idx_to_pmc,
	.is_valid_msr = intel_is_valid_msr,
	.get_msr = intel_pmu_get_msr,
	.set_msr = intel_pmu_set_msr,
	.refresh = intel_pmu_refresh,
	.init = intel_pmu_init,
	.destroy = intel_pmu_destroy,
	.reset = intel_pmu_reset,
	.deliver_pmi = intel_pmu_deliver_pmi,
	.cleanup = intel_pmu_cleanup,
	.put_guest_context = intel_put_guest_context,
	.load_guest_context = intel_load_guest_context,
	.context_switch_need_skip = intel_pmu_context_switch_need_skip,
	.EVENTSEL_EVENT = ARCH_PERFMON_EVENTSEL_EVENT,
	.MAX_NR_GP_COUNTERS = KVM_MAX_NR_INTEL_GP_COUNTERS,
	.MIN_NR_GP_COUNTERS = 1,
	/*
	 * Intel mediated vPMU support depends on
	 * MSR_CORE_PERF_GLOBAL_STATUS_SET which is supported from 4+.
	 */
	.MIN_MEDIATED_PMU_VERSION = 4,
};
