// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 Intel Corporation */
#include "adf_admin.h"
#include "adf_kpt.h"
#include "adf_sysfs_kpt.h"
#include "adf_common_drv.h"
#include "adf_cfg_services.h"
#include "icp_qat_fw_init_admin.h"

struct adf_kpt_cfg {
	unsigned int swk_cnt_per_fn;
	unsigned int swk_cnt_per_pasid;
	unsigned int swk_ttl_in_secs;
	unsigned int swk_shared;
};

static void adf_cfg_kpt_config(struct adf_accel_dev *accel_dev, void *vptr)
{
	struct adf_kpt_interface_data *user_data = GET_KPT_USER_DATA(accel_dev);
	struct adf_kpt_cfg *kpt_config = vptr;

	kpt_config->swk_cnt_per_fn = user_data->swk_cnt_per_fn;
	kpt_config->swk_cnt_per_pasid = user_data->swk_cnt_per_pasid;
	kpt_config->swk_ttl_in_secs = user_data->swk_max_ttl;
	kpt_config->swk_shared = user_data->swk_shared;
}

int adf_enable_kpt(struct adf_accel_dev *accel_dev)
{
	struct adf_kpt_interface_data *user_data = GET_KPT_USER_DATA(accel_dev);
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);
	bool is_kpt_capable = false;
	dma_addr_t ptr;
	u32 svc_mask;
	void *vptr;
	int ret;

	/*
	 * Cleanup KPT capability mask before enabling KPT
	 */
	hw_data->accel_capabilities_mask &= ~ICP_ACCEL_CAPABILITIES_KPT;

	/*
	 * Check KPT hardware capability
	 */
	if (hw_data->kpt_capable)
		is_kpt_capable = hw_data->kpt_capable(accel_dev);

	if (!is_kpt_capable)
		return 0;

	ret = adf_get_service_enabled(accel_dev, &svc_mask);
	if (ret)
		return ret;
	if (user_data->enable && SVC_ASYM != svc_mask) {
		dev_err(&GET_DEV(accel_dev),
			"KPT can only be enabled when service is configured as 'asym'\n");
		user_data->enable = false;
		return -EINVAL;
	}

	if (user_data->enable) {
		vptr = dma_alloc_coherent(&GET_DEV(accel_dev),
					  PAGE_SIZE, &ptr, GFP_KERNEL);
		if (!vptr)
			return -ENOMEM;

		adf_cfg_kpt_config(accel_dev, vptr);
		ret = adf_init_admin_kpt(accel_dev, ptr,
					 sizeof(struct adf_kpt_cfg));

		dma_free_coherent(&GET_DEV(accel_dev), PAGE_SIZE, vptr, ptr);

		if (ret)
			goto ret_err;

		/*
		 * Update device capabilities to KPT mode's value
		 */
		hw_data->accel_capabilities_mask = hw_data->kpt_data.kpt_mode_dev_cap;
	}

	ret = adf_sysfs_kpt_add(accel_dev);
	if (ret)
		dev_err(&GET_DEV(accel_dev), "failed to add KPT sysfs interface\n");

ret_err:
	return ret;
}
EXPORT_SYMBOL_GPL(adf_enable_kpt);
