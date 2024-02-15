// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 Intel Corporation */
#include <linux/iopoll.h>
#include <adf_accel_devices.h>
#include <adf_admin.h>
#include <adf_bank_state.h>
#include <adf_cfg.h>
#include <adf_cfg_services.h>
#include <adf_clock.h>
#include <adf_common_drv.h>
#include <adf_fw_config.h>
#include <adf_gen6_pm.h>
#include <adf_gen6_ras.h>
#include <adf_gen6_shared.h>
#include <adf_gen6_tl.h>
#include <adf_ras.h>
#include <adf_timer.h>
#include "adf_6xxx_hw_data.h"
#include "icp_qat_fw_comp.h"
#include "icp_qat_hw_51_comp.h"
#include "qat_crypto.h"

#define RP_GROUP_0_MASK		(BIT(0) | BIT(2))
#define RP_GROUP_1_MASK		(BIT(1) | BIT(3))
#define RP_GROUP_ALL_MASK	(RP_GROUP_0_MASK | RP_GROUP_1_MASK)

#define ADF_AE_GROUP_0		GENMASK(3, 0)
#define ADF_AE_GROUP_1		GENMASK(7, 4)
#define ADF_AE_GROUP_2		BIT(8)

struct adf_ring_config {
	u32 ring_mask;
	enum adf_services service;
};

enum adf_gen6_rp_svc {
	SINGLE_SVC = 1,
	DOUBLE_SVC,
	TRIPLE_SVC,
};

enum adf_gen6_rps {
	RP0 = 0,
	RP1,
	RP2,
	RP3,
};

/*
 * Thread bitmask per service for all AE.
 * Bit position will indicate the thread id.
 * Bit value:
 * 1 - thread will handle the service.
 * 0 - thread will not handle the service.
 */
static const unsigned long thrd_mask_sym[ADF_6XXX_MAX_ACCELENGINES] = {
	0xC, 0xC, 0xC, 0xC, 0x1C, 0x1C, 0x1C, 0x1C, 0
};

static const unsigned long thrd_mask_asym[ADF_6XXX_MAX_ACCELENGINES] = {
	0x70, 0x70, 0x70, 0x70, 0x60, 0x60, 0x60, 0x60, 0
};

static const unsigned long thrd_mask_cpr[ADF_6XXX_MAX_ACCELENGINES] = {
	0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0
};

static const unsigned long thrd_mask_dcc[ADF_6XXX_MAX_ACCELENGINES] = {
	0, 0, 0, 0, 0x7, 0x7, 0x3, 0x3, 0
};

static const unsigned long thrd_mask_dcpr[ADF_6XXX_MAX_ACCELENGINES] = {
	0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0x2, 0
};

static bool enable_hb_sync;

static const char *const adf_6xxx_fw_objs[] = {
	[ADF_FW_CY_OBJ] = ADF_6XXX_CY_OBJ,
	[ADF_FW_DC_OBJ] = ADF_6XXX_DC_OBJ,
	[ADF_FW_ADMIN_OBJ] = ADF_6XXX_ADMIN_OBJ
};

static const struct adf_fw_config adf_default_fw_ae_config[] = {
	{ ADF_AE_GROUP_1, ADF_FW_DC_OBJ },
	{ ADF_AE_GROUP_0, ADF_FW_CY_OBJ },
	{ ADF_AE_GROUP_2, ADF_FW_ADMIN_OBJ },
};

static struct adf_hw_device_class adf_6xxx_class = {
	.name = ADF_6XXX_DEVICE_NAME,
	.type = DEV_6XXX,
	.instances = 0,
};

static int adf_gen6_service_supported(u32 service_mask)
{
	int num_svc = hweight32(service_mask);

	if (service_mask >= BIT(SVC_ID_COUNT))
		return -EINVAL;

	switch (num_svc) {
	case SINGLE_SVC:
		return 0;
	case DOUBLE_SVC:
	case TRIPLE_SVC:
		if (service_mask & SVC_DCC)
			return -EINVAL;
		return 0;
	default:
		return -EINVAL;
	}
}

static bool get_rp_config(struct adf_accel_dev *accel_dev,
			  struct adf_ring_config **rp_config,
			  u32 *num_grp)
{
	static struct adf_ring_config adf_rp_config[MAX_NUM_CONCURR_SVC];
	unsigned long svc_mask = 0;
	u32 svc = 0, mask = 0;
	u8 pke_pair;
	int i = 0;

	if (adf_get_service_enabled(accel_dev, &mask))
		return false;
	svc_mask = mask;
	for_each_set_bit(svc, &svc_mask, SVC_ID_COUNT) {
		adf_rp_config[i].ring_mask = 1 << i;

		switch (BIT(svc)) {
		case SVC_SYM:
			adf_rp_config[i].service = SVC_ID_SYM;
			break;
		case SVC_ASYM:
			adf_rp_config[i].service = SVC_ID_ASYM;
			pke_pair = i;
			break;
		case SVC_DC:
			adf_rp_config[i].service = SVC_ID_DC;
			break;
		case SVC_DCC:
			adf_rp_config[i].service = SVC_ID_DCC;
			break;
		case SVC_DECOMP:
			adf_rp_config[i].service = SVC_ID_DECOMP;
			break;
		default:
			return false;
		}
		i++;
		*num_grp = i;
	}

	/*
	 * Service configurations that enable 3 services concurrently
	 * are not supported in kernel space, i.e. configuration
	 * consisting of 3 services will not result in creation of any
	 * kernel instances.
	 * 3 service configurations are however supported in user space
	 * and map to user space instances.
	 */

	switch (*num_grp) {
	case SINGLE_SVC:
		adf_rp_config[RP0].ring_mask = RP_GROUP_ALL_MASK;
		break;
	case DOUBLE_SVC:
		adf_rp_config[RP0].ring_mask = RP_GROUP_0_MASK;
		adf_rp_config[RP1].ring_mask = RP_GROUP_1_MASK;
		break;
	case TRIPLE_SVC:
		if (svc_mask & SVC_ASYM)
			adf_rp_config[pke_pair].ring_mask |= 1 << RP3;
		break;
	default:
		return false;
	}

	*rp_config = adf_rp_config;

	return true;
}

static u32 adf_gen6_get_arb_mask(struct adf_accel_dev *accel_dev, u32 accel_id)
{
	struct adf_ring_config *rp_config;
	u32 num_grp = 0, id, thrd;
	u32 thd2arb_mask = 0;
	bool sts;

	sts = get_rp_config(accel_dev, &rp_config, &num_grp);
	if (!sts)
		return 0;

	for (id = 0; id < num_grp; id++) {
		switch (rp_config[id].service) {
		case SVC_ID_ASYM:
			for_each_set_bit(thrd, &thrd_mask_asym[accel_id],
					 ADF_NUM_THREADS_PER_AE)
				thd2arb_mask |=
					(rp_config[id].ring_mask << (thrd * 4));
			break;
		case SVC_ID_SYM:
			for_each_set_bit(thrd, &thrd_mask_sym[accel_id],
					 ADF_NUM_THREADS_PER_AE)
				thd2arb_mask |=
					(rp_config[id].ring_mask << (thrd * 4));
			break;
		case SVC_ID_DC:
			for_each_set_bit(thrd, &thrd_mask_cpr[accel_id],
					 ADF_NUM_THREADS_PER_AE)
				thd2arb_mask |=
					(rp_config[id].ring_mask << (thrd * 4));
			break;
		case SVC_ID_DCC:
			for_each_set_bit(thrd, &thrd_mask_dcc[accel_id],
					 ADF_NUM_THREADS_PER_AE)
				thd2arb_mask |=
				(rp_config[id].ring_mask << (thrd * 4));
			break;
		case SVC_ID_DECOMP:
			for_each_set_bit(thrd, &thrd_mask_dcpr[accel_id],
					 ADF_NUM_THREADS_PER_AE)
				thd2arb_mask |=
				(rp_config[id].ring_mask << (thrd * 4));
			break;
		default:
			break;
		}
	}

	return thd2arb_mask;
}

static u16 adf_gen6_get_ring_to_svc_map(struct adf_accel_dev *accel_dev)
{
	enum adf_cfg_service_type rps[ADF_GEN6_NUM_BANKS_PER_VF], svc;
	struct adf_ring_config *rp_config;
	unsigned long cfg_mask;
	u16 ring_to_svc_map;
	u32 num_grp, rp_num;
	bool sts;
	int i;

	sts = get_rp_config(accel_dev, &rp_config, &num_grp);
	if (!sts)
		return 0;

	/*
	 * With 3 concurrent services enabled, 3 RPs are assigned to services
	 * and the last ring pair in each bundle remains unused. However if
	 * PKE is among the enabled services, ADF_FW_ASYM_OBJ will be assigned
	 * to the last ring pair and UNUSED service will be overwritten
	 * with ASYM.
	 */
	if (num_grp == TRIPLE_SVC)
		rps[RP3] = UNUSED;

	for (i = 0; i < num_grp; i++) {
		switch (rp_config[i].service) {
		case SVC_ID_SYM:
			svc = SYM;
			break;
		case SVC_ID_ASYM:
			svc = ASYM;
			break;
		case SVC_ID_DC:
		case SVC_ID_DCC:
			svc = COMP;
			break;
		case SVC_ID_DECOMP:
			svc = DECOMP;
			break;
		default:
			svc = UNUSED;
		}
		cfg_mask = rp_config[i].ring_mask;
		for_each_set_bit(rp_num, &cfg_mask, ADF_GEN6_NUM_BANKS_PER_VF)
			rps[rp_num] = svc;
	}

	ring_to_svc_map = rps[RP0] << ADF_CFG_SERV_RING_PAIR_0_SHIFT |
		rps[RP1] << ADF_CFG_SERV_RING_PAIR_1_SHIFT |
		rps[RP2] << ADF_CFG_SERV_RING_PAIR_2_SHIFT |
		rps[RP3] << ADF_CFG_SERV_RING_PAIR_3_SHIFT;

	return ring_to_svc_map;
}

static u32 adf_gen6_get_accel_mask(struct adf_hw_device_data *self)
{
	return ADF_GEN6_ACCELERATORS_MASK;
}

static u32 adf_gen6_get_num_accels(struct adf_hw_device_data *self)
{
	return ADF_GEN6_MAX_ACCELERATORS;
}

static u32 adf_gen6_get_num_aes(struct adf_hw_device_data *self)
{
	if (!self || !self->ae_mask)
		return 0;

	return hweight32(self->ae_mask);
}

static u32 adf_gen6_get_misc_bar_id(struct adf_hw_device_data *self)
{
	return ADF_GEN6_PMISC_BAR;
}

static u32 adf_gen6_get_etr_bar_id(struct adf_hw_device_data *self)
{
	return ADF_GEN6_ETR_BAR;
}

static u32 adf_gen6_get_sram_bar_id(struct adf_hw_device_data *self)
{
	return ADF_GEN6_SRAM_BAR;
}

static enum dev_sku_info adf_gen6_get_sku(struct adf_hw_device_data *self)
{
	return DEV_SKU_1;
}

static void adf_gen6_get_arb_info(struct arb_info *arb_info)
{
	arb_info->arb_cfg = ADF_GEN6_ARB_CONFIG;
	arb_info->arb_offset = ADF_GEN6_ARB_OFFSET;
	arb_info->wt2sam_offset = ADF_GEN6_ARB_WRK_2_SER_MAP_OFFSET;
}

static void adf_gen6_get_admin_info(struct admin_info *admin_csrs_info)
{
	admin_csrs_info->mailbox_offset = ADF_GEN6_MAILBOX_BASE_OFFSET;
	admin_csrs_info->admin_msg_ur = ADF_GEN6_ADMINMSGUR_OFFSET;
	admin_csrs_info->admin_msg_lr = ADF_GEN6_ADMINMSGLR_OFFSET;
}

static u32 adf_gen6_get_heartbeat_clock(struct adf_hw_device_data *self)
{
	/*
	 * GEN6 uses KPT counter for heartbeat
	 */
	return ADF_GEN6_KPT_COUNTER_FREQ;
}

static void adf_gen6_enable_error_correction(struct adf_accel_dev *accel_dev)
{
	struct adf_bar *misc_bar = &GET_BARS(accel_dev)[ADF_GEN6_PMISC_BAR];
	void __iomem *csr = misc_bar->virt_addr;

	/*
	 * Enable all error notification bits in errsou3 except VFLR
	 * notification on host
	 */
	ADF_CSR_WR(csr, ADF_GEN6_ERRMSK3, ADF_GEN6_VFLNOTIFY);
}

static void adf_gen6_enable_ints(struct adf_accel_dev *accel_dev)
{
	void __iomem *addr;

	addr = (&GET_BARS(accel_dev)[ADF_GEN6_PMISC_BAR])->virt_addr;

	/*
	 * Enable bundle interrupts
	 */
	ADF_CSR_WR(addr, ADF_GEN6_SMIAPF_RP_X0_MASK_OFFSET, 0);
	ADF_CSR_WR(addr, ADF_GEN6_SMIAPF_RP_X1_MASK_OFFSET, 0);

	/*
	 * Enable misc interrupts
	 */
	ADF_CSR_WR(addr, ADF_GEN6_SMIAPF_MASK_OFFSET, 0);
}

static inline void adf_gen6_unpack_ssm_wdtimer(u64 value, u32 *upper,
					       u32 *lower)
{
	*lower = lower_32_bits(value);
	*upper = upper_32_bits(value);
}

static void adf_gen6_set_ssm_wdtimer(struct adf_accel_dev *accel_dev)
{
	void __iomem *pmisc_addr = adf_get_pmisc_base(accel_dev);
	u64 timer_val_pke = ADF_SSM_WDT_PKE_DEFAULT_VALUE;
	u64 timer_val = ADF_SSM_WDT_DEFAULT_VALUE;
	u32 ssm_wdt_pke_high = 0;
	u32 ssm_wdt_pke_low = 0;
	u32 ssm_wdt_high = 0;
	u32 ssm_wdt_low = 0;

	/*
	 * Convert 64bit watchdog timer value into 32bit values for
	 * mmio write to 32bit CSRs.
	 */
	adf_gen6_unpack_ssm_wdtimer(timer_val, &ssm_wdt_high, &ssm_wdt_low);
	adf_gen6_unpack_ssm_wdtimer(timer_val_pke, &ssm_wdt_pke_high,
				    &ssm_wdt_pke_low);

	/* Enable watchdog timer for sym and dc */
	ADF_CSR_WR(pmisc_addr, ADF_SSMWDTATHL_OFFSET, ssm_wdt_low);
	ADF_CSR_WR(pmisc_addr, ADF_SSMWDTATHH_OFFSET, ssm_wdt_high);
	ADF_CSR_WR(pmisc_addr, ADF_SSMWDTCNVL_OFFSET, ssm_wdt_low);
	ADF_CSR_WR(pmisc_addr, ADF_SSMWDTCNVH_OFFSET, ssm_wdt_high);
	ADF_CSR_WR(pmisc_addr, ADF_SSMWDTUCSL_OFFSET, ssm_wdt_low);
	ADF_CSR_WR(pmisc_addr, ADF_SSMWDTUCSH_OFFSET, ssm_wdt_high);
	ADF_CSR_WR(pmisc_addr, ADF_SSMWDTDCPRL_OFFSET, ssm_wdt_low);
	ADF_CSR_WR(pmisc_addr, ADF_SSMWDTDCPRH_OFFSET, ssm_wdt_high);

	/* Enable watchdog timer for pke */
	ADF_CSR_WR(pmisc_addr, ADF_SSMWDTPKEL_OFFSET, ssm_wdt_pke_low);
	ADF_CSR_WR(pmisc_addr, ADF_SSMWDTPKEH_OFFSET, ssm_wdt_pke_high);
}

/*
 * The vector routing table is used to select the MSI-X entry to use for each
 * interrupt source.
 * The first ADF_GEN6_ETR_MAX_BANKS entries correspond to ring interrupts.
 * The final entry corresponds to VF2PF or error interrupts.
 * This vector table could be used to configure one MSI-X entry to be shared
 * between multiple interrupt sources.
 *
 * The default routing is set to have a one to one correspondence between the
 * interrupt source and the MSI-X entry used.
 */
static void adf_gen6_set_msix_default_rttable(struct adf_accel_dev *accel_dev)
{
	void __iomem *csr;
	int i;

	csr = (&GET_BARS(accel_dev)[ADF_GEN6_PMISC_BAR])->virt_addr;
	for (i = 0; i <= ADF_GEN6_ETR_MAX_BANKS; i++)
		ADF_CSR_WR(csr, ADF_GEN6_MSIX_RTTABLE_OFFSET(i), i);
}

static int reset_ring_pair(void __iomem *csr, u32 bank_number)
{
	u32 status;
	int ret;

	/*
	 * Write rpresetctl register BIT(0) as 1
	 * Since rpresetctl registers have no RW fields, no need to preserve
	 * values for other bits. Just write directly.
	 */
	ADF_CSR_WR(csr, ADF_WQM_CSR_RPRESETCTL(bank_number),
		   ADF_WQM_CSR_RPRESETCTL_RESET);

	/* Read rpresetsts register and wait for rp reset to complete */
	ret = read_poll_timeout(ADF_CSR_RD, status,
				status & ADF_WQM_CSR_RPRESETSTS_STATUS,
				ADF_RPRESET_POLL_DELAY_US,
				ADF_RPRESET_POLL_TIMEOUT_US, true,
				csr, ADF_WQM_CSR_RPRESETSTS(bank_number));
	if (!ret) {
		/* When ring pair reset is done, clear rpresetsts */
		ADF_CSR_WR(csr, ADF_WQM_CSR_RPRESETSTS(bank_number),
			   ADF_WQM_CSR_RPRESETSTS_STATUS);
	}

	return ret;
}

static inline bool adf_anti_rb_enabled(struct adf_accel_dev *accel_dev)
{
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);

	if (hw_data->fuse0 & ADF_GEN6_FUSE_ANTI_RB)
		return true;
	return false;
}

static void adf_gen6_init_anti_rb(struct adf_anti_rb_hw_data *anti_rb_data)
{
	anti_rb_data->anti_rb_enabled = adf_anti_rb_enabled;
	anti_rb_data->svncheck_offset = ADF_GEN6_SVNCHECK_CSR_MSG;
	anti_rb_data->svncheck_retry = 0;
	anti_rb_data->sysfs_added = false;
}

static int adf_gen6_ring_pair_reset(struct adf_accel_dev *accel_dev, u32 bank_number)
{
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	void __iomem *csr = adf_get_etr_base(accel_dev);
	int ret;

	if (bank_number >= hw_data->num_banks)
		return -EINVAL;

	dev_dbg(&GET_DEV(accel_dev),
		"ring pair reset for bank:%d\n", bank_number);

	ret = reset_ring_pair(csr, bank_number);
	if (ret)
		dev_err(&GET_DEV(accel_dev),
			"ring pair reset failed (timeout)\n");
	else
		dev_dbg(&GET_DEV(accel_dev), "ring pair reset successful\n");

	return ret;
}

static void adf_gen6_build_comp_dc_hw_block(void **ctx,
					    enum icp_qat_hw_compression_algo algo)
{
	struct icp_qat_fw_comp_req *req_tmpl =
		(struct icp_qat_fw_comp_req *)*ctx;
	struct icp_qat_fw_comp_req_hdr_cd_pars *cd_pars = &req_tmpl->cd_pars;
	struct icp_qat_hw_comp_51_config_csr_lower hw_comp_lower_csr = {0};
	struct icp_qat_fw_comn_req_hdr *header = &req_tmpl->comn_hdr;
	u32 lower_val;

	switch (algo) {
	case ICP_QAT_HW_COMPRESSION_ALGO_DEFLATE:
		header->service_cmd_id = ICP_QAT_FW_COMP_CMD_DYNAMIC;
	break;
	case ICP_QAT_HW_COMPRESSION_ALGO_ZSTD:
		header->service_cmd_id = ICP_QAT_FW_COMP_CMD_ZSTD_COMPRESS;
	break;
	default:
		return;
	}
	hw_comp_lower_csr.lllbd = ICP_QAT_HW_COMP_51_LLLBD_CTRL_LLLBD_DISABLED;
	hw_comp_lower_csr.sd = ICP_QAT_HW_COMP_51_SEARCH_DEPTH_LEVEL_1;
	lower_val = ICP_QAT_FW_COMP_51_BUILD_CONFIG_LOWER(hw_comp_lower_csr);
	cd_pars->u.sl.comp_slice_cfg_word[0] = lower_val;
	cd_pars->u.sl.comp_slice_cfg_word[1] = 0;
}

static void adf_gen6_build_decomp_dc_hw_block(void **ctx,
					      enum icp_qat_hw_compression_algo algo)
{
	struct icp_qat_fw_comp_req *req_tmpl =
		(struct icp_qat_fw_comp_req *)*ctx;
	struct icp_qat_fw_comp_req_hdr_cd_pars *cd_pars = &req_tmpl->cd_pars;
	struct icp_qat_fw_comn_req_hdr *header = &req_tmpl->comn_hdr;

	switch (algo) {
	case ICP_QAT_HW_COMPRESSION_ALGO_DEFLATE:
		header->service_cmd_id = ICP_QAT_FW_COMP_CMD_DECOMPRESS;
	break;
	case ICP_QAT_HW_COMPRESSION_ALGO_ZSTD:
		header->service_cmd_id = ICP_QAT_FW_COMP_CMD_ZSTD_DECOMPRESS;
	break;
	default:
		return;
	}
	cd_pars->u.sl.comp_slice_cfg_word[0] = 0;
	cd_pars->u.sl.comp_slice_cfg_word[1] = 0;
}

static void adf_gen6_init_dc_ops(struct adf_dc_ops *dc_ops)
{
	dc_ops->build_comp_dc_hw_block = adf_gen6_build_comp_dc_hw_block;
	dc_ops->build_decomp_dc_hw_block = adf_gen6_build_decomp_dc_hw_block;
}

static int adf_gen6_init_thd2arb_map(struct adf_accel_dev *accel_dev)
{
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);
	u32 *thd2arb_map = hw_data->thd_to_arb_map;
	u32 arb_mask;
	u8 me_num;

	if (!hw_data->num_engines)
		return -EFAULT;

	for (me_num = 0; me_num < hw_data->num_engines; me_num++) {
		arb_mask = adf_gen6_get_arb_mask(accel_dev, me_num);
		thd2arb_map[me_num] = arb_mask;
		dev_dbg(&GET_DEV(accel_dev), "ME:%d arb_mask:%#x\n", me_num,
			arb_mask);
	}

	return 0;
}

static u32 adf_gen6_get_num_svc_aes(struct adf_accel_dev *accel_dev,
				    enum adf_cfg_service_type svc_type)
{
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);
	u32 obj_num, num_grp, ae_mask;
	int obj_type, obj_iter;

	switch (svc_type) {
	case SYM:
	case ASYM:
		obj_type = ADF_FW_CY_OBJ;
		break;
	case COMP:
		obj_type = ADF_FW_DC_OBJ;
		break;
	case DECOMP:
		obj_type = ADF_FW_DC_OBJ;
		break;
	default:
		return 0;
	}
	num_grp = hw_data->uof_get_num_objs(accel_dev);
	for (obj_num = 0; obj_num < num_grp; obj_num++) {
		obj_iter = hw_data->uof_get_obj_type(accel_dev, obj_num);
		if (obj_iter == obj_type) {
			ae_mask = hw_data->uof_get_ae_mask(accel_dev, obj_num);
			return hweight32(ae_mask);
		}
	}
	return 0;
}

static u32 adf_gen6_get_rl_svc_slice_cnt(enum adf_cfg_service_type svc,
					 struct rl_slice_cnt *slices)
{
	switch (svc) {
	case SYM:
		return slices->cph_cnt;
	case ASYM:
		return slices->pke_cnt;
	case COMP:
		return slices->cpr_cnt + slices->dcpr_cnt;
	case DECOMP:
		return slices->dcpr_cnt;
	default:
		return 0;
	}
}

static void set_vc_csr_for_bank(void __iomem *csr, u32 bank_number)
{
	u32 value;

	/*
	 * After each PF FLR, for each of the 64 ring pairs in the PF,
	 * driver must program the ringmodectl CSRs.
	 * Read RINGMODECTL then write masked values:
	 */
	value = ADF_CSR_RD(csr, ADF_GEN6_CSR_RINGMODECTL(bank_number));
	value &= ~ADF_GEN6_RINGMODECTL_TC_MASK;
	value |= (ADF_GEN6_RINGMODECTL_TC_DEFAULT << ADF_GEN6_RINGMODECTL_TC_OFFSET);
	value &= ~ADF_GEN6_RINGMODECTL_TC_EN_MASK;
	value |= (ADF_GEN6_RINGMODECTL_TC_EN_OP1 << ADF_GEN6_RINGMODECTL_TC_EN_OFFSET);
	ADF_CSR_WR(csr, ADF_GEN6_CSR_RINGMODECTL(bank_number), value);
}

static int set_vc_config(struct adf_accel_dev *accel_dev)
{
	struct pci_dev *pdev = accel_to_pci_dev(accel_dev);
	u32 value;
	int err;

	/*
	 * After each PF FLR, the driver must program the Port Virtual Channel (VC)
	 * Control Registers.
	 * Read PVC0CTL then write masked values:
	 */
	pci_read_config_dword(pdev, ADF_GEN6_PVC0CTL_OFFSET, &value);
	value &= ~ADF_GEN6_PVC0CTL_TCVCMAP_MASK;
	value |= (ADF_GEN6_PVC0CTL_TCVCMAP_DEFAULT << ADF_GEN6_PVC0CTL_TCVCMAP_OFFSET);
	err = pci_write_config_dword(pdev, ADF_GEN6_PVC0CTL_OFFSET, value);
	if (err) {
		dev_err(&GET_DEV(accel_dev), "pci write to PVC0CTL failed\n");
		goto out_err;
	}

	/*
	 * Read PVC1CTL then write masked values:
	 */
	pci_read_config_dword(pdev, ADF_GEN6_PVC1CTL_OFFSET, &value);
	value &= ~ADF_GEN6_PVC1CTL_TCVCMAP_MASK;
	value |= (ADF_GEN6_PVC1CTL_TCVCMAP_DEFAULT << ADF_GEN6_PVC1CTL_TCVCMAP_OFFSET);
	value &= ~ADF_GEN6_PVC1CTL_VCEN_MASK;
	value |= (ADF_GEN6_PVC1CTL_VCEN_ON << ADF_GEN6_PVC1CTL_VCEN_OFFSET);
	err = pci_write_config_dword(pdev, ADF_GEN6_PVC1CTL_OFFSET, value);
	if (err)
		dev_err(&GET_DEV(accel_dev), "pci write to PVC1CTL failed\n");

out_err:
	return err;
}

static int adf_gen6_set_vc(struct adf_accel_dev *accel_dev)
{
	struct adf_hw_device_data *hw_data = accel_dev->hw_device;
	void __iomem *csr;
	u32 bank_number;

	csr = adf_get_etr_base(accel_dev);

	for (bank_number = 0; bank_number < hw_data->num_banks; bank_number++) {
		dev_dbg(&GET_DEV(accel_dev),
			"set virtual channels for bank:%d\n", bank_number);

		set_vc_csr_for_bank(csr, bank_number);
	}

	return set_vc_config(accel_dev);
}

static void adf_gen6_set_crypto_cap(struct adf_accel_dev *accel_dev)
{
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);

	hw_data->crypto_cipher_caps = AES_XTS | AES_CTR;
	hw_data->crypto_aead_caps = 0;
	hw_data->aes_192_fallback = true;
}

static void get_fw_ae_config(const struct adf_fw_config **fw_config,
			     u32 *num_grp)
{
	*fw_config = adf_default_fw_ae_config;
	*num_grp = ARRAY_SIZE(adf_default_fw_ae_config);
}

static u32 get_ae_mask(struct adf_hw_device_data *self)
{
	u32 ae_disable = self->fuses;

	return ~ae_disable & ADF_6XXX_ACCELENGINES_MASK;
}

static void set_comp_cap(struct adf_accel_dev *accel_dev)
{
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);

	hw_data->zstd_supported = true;
}

static u32 get_accel_cap(struct adf_accel_dev *accel_dev)
{
	struct pci_dev *pdev = accel_dev->accel_pci_dev.pci_dev;
	u32 capabilities_sym, capabilities_asym;
	u32 capabilities_dc;
	u32 caps = 0;
	u32 fusectl1;
	u32 svc_mask;

	/* Read accelerator capabilities mask */
	pci_read_config_dword(pdev, ADF_GEN6_FUSECTL1_OFFSET, &fusectl1);

	capabilities_sym = ICP_ACCEL_CAPABILITIES_CRYPTO_SYMMETRIC |
			  ICP_ACCEL_CAPABILITIES_CIPHER |
			  ICP_ACCEL_CAPABILITIES_AUTHENTICATION |
			  ICP_ACCEL_CAPABILITIES_SHA3 |
			  ICP_ACCEL_CAPABILITIES_SHA3_EXT |
			  ICP_ACCEL_CAPABILITIES_CHACHA_POLY |
			  ICP_ACCEL_CAPABILITIES_AESGCM_SPC |
			  ICP_ACCEL_CAPABILITIES_AES_V2;

	/*
	 * A set bit in fusectl1 means the corresponding feature
	 * is OFF in this SKU
	 */
	if (fusectl1 & ICP_ACCEL_GEN6_MASK_UCS_SLICE) {
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_CRYPTO_SYMMETRIC;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_CIPHER;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_CHACHA_POLY;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_AESGCM_SPC;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_AES_V2;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_CIPHER;
	}
	if (fusectl1 & ICP_ACCEL_GEN6_MASK_AUTH_SLICE) {
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_AUTHENTICATION;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_SHA3;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_SHA3_EXT;
		capabilities_sym &= ~ICP_ACCEL_CAPABILITIES_CIPHER;
	}
	capabilities_asym = ICP_ACCEL_CAPABILITIES_CRYPTO_ASYMMETRIC |
			  ICP_ACCEL_CAPABILITIES_CIPHER |
			  ICP_ACCEL_CAPABILITIES_SM2 |
			  ICP_ACCEL_CAPABILITIES_ECEDMONT;

	if (fusectl1 & ICP_ACCEL_GEN6_MASK_PKE_SLICE) {
		capabilities_asym &= ~ICP_ACCEL_CAPABILITIES_CRYPTO_ASYMMETRIC;
		capabilities_asym &= ~ICP_ACCEL_CAPABILITIES_SM2;
		capabilities_asym &= ~ICP_ACCEL_CAPABILITIES_ECEDMONT;
	}

	capabilities_dc = ICP_ACCEL_CAPABILITIES_COMPRESSION |
			  ICP_ACCEL_CAPABILITIES_LZ4_COMPRESSION |
			  ICP_ACCEL_CAPABILITIES_LZ4S_COMPRESSION |
			  ICP_ACCEL_CAPABILITIES_CNV_INTEGRITY64;

	if (fusectl1 & ICP_ACCEL_GEN6_MASK_CPR_SLICE) {
		capabilities_dc &= ~ICP_ACCEL_CAPABILITIES_COMPRESSION;
		capabilities_dc &= ~ICP_ACCEL_CAPABILITIES_LZ4_COMPRESSION;
		capabilities_dc &= ~ICP_ACCEL_CAPABILITIES_LZ4S_COMPRESSION;
		capabilities_dc &= ~ICP_ACCEL_CAPABILITIES_CNV_INTEGRITY64;
	}

	if (adf_get_service_enabled(accel_dev, &svc_mask))
		return 0;
	if (svc_mask & SVC_ASYM)
		caps |= capabilities_asym;
	if (svc_mask & SVC_SYM)
		caps |= capabilities_sym;
	if (svc_mask & SVC_DC || svc_mask & SVC_DECOMP)
		caps |= capabilities_dc;
	if (svc_mask & SVC_DCC) {
		/*
		 * Sym capabilities are available for chaining operations,
		 * but sym crypto instances cannot be supported
		 */
		caps = capabilities_dc | capabilities_sym;
		caps &= ~ICP_ACCEL_CAPABILITIES_CRYPTO_SYMMETRIC;
	}

	return caps;
}

static u32 uof_get_num_objs(struct adf_accel_dev *accel_dev)
{
	return ARRAY_SIZE(adf_default_fw_ae_config);
}

static const char *uof_get_name(struct adf_accel_dev *accel_dev, u32 obj_num,
				const char * const fw_objs[], int num_objs)
{
	int id;

	id = adf_default_fw_ae_config[obj_num].obj;
	if (id >= num_objs)
		return NULL;

	return fw_objs[id];
}

static const char *uof_get_name_6xxx(struct adf_accel_dev *accel_dev, u32 obj_num)
{
	int num_fw_objs = ARRAY_SIZE(adf_6xxx_fw_objs);

	return uof_get_name(accel_dev, obj_num, adf_6xxx_fw_objs, num_fw_objs);
}

static int uof_get_obj_type(struct adf_accel_dev *accel_dev, u32 obj_num)
{
	const struct adf_fw_config *fw_config;
	u32 num_grp;

	if (obj_num >= uof_get_num_objs(accel_dev))
		return -EINVAL;

	get_fw_ae_config(&fw_config, &num_grp);

	return fw_config[obj_num].obj;
}

static u32 uof_get_ae_mask(struct adf_accel_dev *accel_dev, u32 obj_num)
{
	const struct adf_fw_config *fw_config = NULL;
	u32 num_grp = 0;

	get_fw_ae_config(&fw_config, &num_grp);
	return fw_config[obj_num].ae_mask;
}

static const u32 *adf_get_arbiter_mapping(struct adf_accel_dev *accel_dev)
{
	if (adf_gen6_init_thd2arb_map(accel_dev))
		dev_warn(&GET_DEV(accel_dev),
			 "Failed to generate thread to arbiter mapping");

	return GET_HW_DATA(accel_dev)->thd_to_arb_map;
}

static int adf_init_device(struct adf_accel_dev *accel_dev)
{
	void __iomem *addr;
	u32 status;
	u32 csr;
	int ret;

	addr = (&GET_BARS(accel_dev)[ADF_GEN6_PMISC_BAR])->virt_addr;

	/* Temporarily mask PM interrupt */
	csr = ADF_CSR_RD(addr, ADF_GEN6_ERRMSK2);
	csr |= ADF_GEN6_PM_SOU;
	ADF_CSR_WR(addr, ADF_GEN6_ERRMSK2, csr);

	/* Set DRV_ACTIVE bit to power up the device */
	ADF_CSR_WR(addr, ADF_GEN6_PM_INTERRUPT, ADF_GEN6_PM_DRV_ACTIVE);

	/* Poll status register to make sure the device is powered up */
	ret = read_poll_timeout(ADF_CSR_RD, status,
				status & ADF_GEN6_PM_INIT_STATE,
				ADF_GEN6_PM_POLL_DELAY_US,
				ADF_GEN6_PM_POLL_TIMEOUT_US, true, addr,
				ADF_GEN6_PM_STATUS);
	if (ret)
		dev_err(&GET_DEV(accel_dev), "Failed to power up the device\n");

	return ret;
}

static bool adf_kpt_capable(struct adf_accel_dev *accel_dev)
{
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);

	if (hw_data->fuse0 & ADF_6XXX_FUSE_KPT)
		return false;
	else
		return true;
}

static void adf_gen6_init_kpt(struct adf_kpt_hw_data *kpt_data)
{
	kpt_data->cfg_max_swk_cnt_per_fn_pasid = ADF_6XXX_KPT_MAX_SWK_COUNT_PER_FNPASID;
	kpt_data->cfg_max_swk_ttl = ADF_6XXX_KPT_MAX_SWK_TTL;

	/*
	 * In KPT mode, keep KPT related capabilities only
	 */
	kpt_data->kpt_mode_dev_cap = ICP_ACCEL_CAPABILITIES_CRYPTO_ASYMMETRIC |
				     ICP_ACCEL_CAPABILITIES_KPT;
	kpt_data->sysfs_added = false;
	kpt_data->user_input.enable = ADF_6XXX_KPT_DEFAULT_ENABLEMENT;
	kpt_data->user_input.swk_shared = ADF_6XXX_KPT_DEFAULT_SWK_SHARED_MODE;
	kpt_data->user_input.swk_max_ttl = ADF_6XXX_KPT_DEFAULT_SWK_TTL;
	kpt_data->user_input.swk_cnt_per_fn = ADF_6XXX_KPT_DEFAULT_SWK_CNT_PER_FN;
	kpt_data->user_input.swk_cnt_per_pasid = ADF_6XXX_KPT_DEFAULT_SWK_CNT_PER_PASID;
}

static void adf_gen6_set_err_mask(struct adf_dev_err_mask *err_mask)
{
	err_mask->cppagentcmdpar_mask = ADF_6XXX_HICPPAGENTCMDPARERRLOG_MASK;
}

static void adf_gen6_set_cmdq_cnt(struct adf_accel_dev *accel_dev)
{
	struct icp_qat_fw_init_admin_slice_cnt *slice_cnt =
		&accel_dev->telemetry->slice_cnt;
	struct icp_qat_fw_init_admin_slice_cnt *cmdq_cnt =
		&accel_dev->telemetry->cmdq_cnt;

	cmdq_cnt->cpr_cnt = slice_cnt->cpr_cnt * 5;
	cmdq_cnt->dcpr_cnt = slice_cnt->dcpr_cnt * 3;
	cmdq_cnt->pke_cnt = slice_cnt->pke_cnt;
	cmdq_cnt->wat_cnt = slice_cnt->wat_cnt * 7;
	cmdq_cnt->wcp_cnt = slice_cnt->wcp_cnt * 7;
	cmdq_cnt->ucs_cnt = slice_cnt->ucs_cnt * 3;
	cmdq_cnt->ath_cnt = slice_cnt->ath_cnt * 2;
}

static void adf_gen6_init_rl_data(struct adf_rl_hw_data *rl_data)
{
	rl_data->pciout_tb_offset = ADF_GEN6_RL_TOKEN_PCIEOUT_BUCKET_OFFSET;
	rl_data->pciin_tb_offset = ADF_GEN6_RL_TOKEN_PCIEIN_BUCKET_OFFSET;
	rl_data->r2l_offset = ADF_GEN6_RL_R2L_OFFSET;
	rl_data->l2c_offset = ADF_GEN6_RL_L2C_OFFSET;
	rl_data->c2s_offset = ADF_GEN6_RL_C2S_OFFSET;
	rl_data->pcie_scale_div = ADF_6XXX_RL_PCIE_SCALE_FACTOR_DIV;
	rl_data->pcie_scale_mul = ADF_6XXX_RL_PCIE_SCALE_FACTOR_MUL;
	rl_data->max_tp[ADF_SVC_ASYM] = ADF_6XXX_RL_MAX_TP_ASYM;
	rl_data->max_tp[ADF_SVC_SYM] = ADF_6XXX_RL_MAX_TP_SYM;
	rl_data->max_tp[ADF_SVC_DC] = ADF_6XXX_RL_MAX_TP_DC;
	rl_data->max_tp[ADF_SVC_DECOMP] = ADF_6XXX_RL_MAX_TP_DECOMP;
	rl_data->scan_interval = ADF_6XXX_RL_SCANS_PER_SEC;
	rl_data->scale_ref = ADF_6XXX_RL_SLICE_REF;
}

void adf_init_hw_data_6xxx(struct adf_hw_device_data *hw_data)
{
	hw_data->dev_class = &adf_6xxx_class;
	hw_data->instance_id = adf_6xxx_class.instances++;
	hw_data->num_banks = ADF_GEN6_ETR_MAX_BANKS;
	hw_data->num_banks_per_vf = ADF_GEN6_NUM_BANKS_PER_VF;
	hw_data->num_rings_per_bank = ADF_GEN6_NUM_RINGS_PER_BANK;
	hw_data->num_accel = ADF_GEN6_MAX_ACCELERATORS;
	hw_data->num_engines = ADF_6XXX_MAX_ACCELENGINES;
	hw_data->num_logical_accel = 1;
	hw_data->tx_rx_gap = ADF_GEN6_RX_RINGS_OFFSET;
	hw_data->tx_rings_mask = ADF_GEN6_TX_RINGS_MASK;
	hw_data->ring_to_svc_map = ADF_GEN6_DEFAULT_RING_TO_SRV_MAP;
	hw_data->alloc_irq = adf_isr_resource_alloc;
	hw_data->free_irq = adf_isr_resource_free;
	hw_data->enable_error_correction = adf_gen6_enable_error_correction;
	hw_data->get_accel_mask = adf_gen6_get_accel_mask;
	hw_data->get_ae_mask = get_ae_mask;
	hw_data->get_num_accels = adf_gen6_get_num_accels;
	hw_data->get_num_aes = adf_gen6_get_num_aes;
	hw_data->get_sram_bar_id = adf_gen6_get_sram_bar_id;
	hw_data->get_etr_bar_id = adf_gen6_get_etr_bar_id;
	hw_data->get_misc_bar_id = adf_gen6_get_misc_bar_id;
	hw_data->get_arb_info = adf_gen6_get_arb_info;
	hw_data->get_admin_info = adf_gen6_get_admin_info;
	hw_data->get_accel_cap = get_accel_cap;
	hw_data->set_comp_cap = set_comp_cap;
	hw_data->get_sku = adf_gen6_get_sku;
	hw_data->init_admin_comms = adf_init_admin_comms;
	hw_data->exit_admin_comms = adf_exit_admin_comms;
	hw_data->send_admin_init = adf_send_admin_init;
	hw_data->init_arb = adf_init_arb;
	hw_data->exit_arb = adf_exit_arb;
	hw_data->get_arb_mapping = adf_get_arbiter_mapping;
	hw_data->enable_ints = adf_gen6_enable_ints;
	hw_data->reset_device = adf_reset_flr;
	hw_data->admin_ae_mask = ADF_6XXX_ADMIN_AE_MASK;
	hw_data->fw_name = ADF_6XXX_FW;
	hw_data->fw_mmp_name = ADF_6XXX_MMP;
	hw_data->uof_get_name = uof_get_name_6xxx;
	hw_data->uof_get_num_objs = uof_get_num_objs;
	hw_data->uof_get_obj_type = uof_get_obj_type;
	hw_data->uof_get_ae_mask = uof_get_ae_mask;
	hw_data->set_msix_rttable = adf_gen6_set_msix_default_rttable;
	hw_data->set_ssm_wdtimer = adf_gen6_set_ssm_wdtimer;
	hw_data->get_ring_to_svc_map = adf_gen6_get_ring_to_svc_map;
	hw_data->disable_iov = adf_disable_sriov;
	hw_data->ring_pair_reset = adf_gen6_ring_pair_reset;
	hw_data->bank_state_save = adf_bank_state_save;
	hw_data->bank_state_restore = adf_bank_state_restore;
	hw_data->set_vc = adf_gen6_set_vc;
	hw_data->dev_config = adf_gen6_dev_config;
	hw_data->get_hb_clock = adf_gen6_get_heartbeat_clock;
	hw_data->num_hb_ctrs = ADF_NUM_HB_CNT_PER_AE;
	if (enable_hb_sync) {
		hw_data->start_timer = adf_timer_start;
		hw_data->stop_timer = adf_timer_stop;
	}
	hw_data->init_device = adf_init_device;
	hw_data->enable_pm = adf_gen6_enable_pm;
	hw_data->service_supported = adf_gen6_service_supported;
	hw_data->start_ras_timer = adf_ras_uncorrectable_timer_start;
	hw_data->stop_ras_timer = adf_ras_uncorrectable_timer_stop;
	hw_data->num_rps = ADF_GEN6_ETR_MAX_BANKS;
	hw_data->set_cmdq_cnt = adf_gen6_set_cmdq_cnt;
	hw_data->clock_frequency = ADF_6XXX_AE_FREQ;
	hw_data->get_num_svc_aes = adf_gen6_get_num_svc_aes;
	hw_data->get_rl_svc_slice_cnt = adf_gen6_get_rl_svc_slice_cnt;
	hw_data->get_rl_sla_val = adf_rl_get_sla_val;
	hw_data->set_crypto_cap = adf_gen6_set_crypto_cap;
	hw_data->kpt_capable = adf_kpt_capable;

	adf_gen6_init_hw_csr_ops(&hw_data->csr_ops);
	adf_gen6_init_pf_pfvf_ops(&hw_data->pfvf_ops);
	adf_gen6_init_dc_ops(&hw_data->dc_ops);
	adf_gen6_init_vf_mig_ops(&hw_data->vfmig_ops);
	adf_gen6_init_ras_ops(&hw_data->ras_ops);
	adf_gen6_set_err_mask(&hw_data->dev_err_mask);
	adf_gen6_init_tl_data(&hw_data->tl_data);
	adf_gen6_init_rl_data(&hw_data->rl_data);
	adf_gen6_init_anti_rb(&hw_data->anti_rb_data);
	adf_gen6_init_kpt(&hw_data->kpt_data);
}

void adf_clean_hw_data_6xxx(struct adf_hw_device_data *hw_data)
{
	hw_data->dev_class->instances--;
}

module_param(enable_hb_sync, bool, 0644);
MODULE_PARM_DESC(enable_hb_sync, "Enable FW heartbeat sync timer");
