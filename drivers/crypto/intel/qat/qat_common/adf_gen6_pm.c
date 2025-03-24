// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 Intel Corporation */
#include "adf_admin.h"
#include "adf_common_drv.h"
#include "adf_gen6_pm.h"

int adf_gen6_enable_pm(struct adf_accel_dev *accel_dev)
{
	int ret;

	ret = adf_init_admin_pm(accel_dev, ADF_GEN6_PM_DEFAULT_IDLE_FILTER);
	if (ret)
		return ret;

	/* Initialize PM internal data */
	adf_gen6_init_dev_pm_data(accel_dev);

	return 0;
}
EXPORT_SYMBOL_GPL(adf_gen6_enable_pm);
