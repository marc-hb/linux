// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 Intel Corporation */
#include <linux/kstrtox.h>
#include <linux/types.h>

#include "adf_accel_devices.h"
#include "adf_cfg.h"
#include "adf_cfg_strings.h"
#include "adf_ring_queue.h"

int adf_ring_queue_set_mode(struct adf_accel_dev *accel_dev,
			    enum adf_ring_queue_mode ring_queue_mode)
{
	unsigned long config_val = ring_queue_mode;
	int ret;

	ret = adf_cfg_add_key_value_param(accel_dev, ADF_GENERAL_SEC,
					  ADF_RING_QUEUE_MODE, &config_val,
					  ADF_DEC);
	if (ret)
		return ret;

	accel_dev->ring_queue_mode = ring_queue_mode;
	return 0;
}

int adf_ring_queue_enable_uq(struct adf_accel_dev *accel_dev)
{
	return adf_ring_queue_set_mode(accel_dev, ADF_RING_QUEUE_UQ);
}

int adf_ring_queue_disable_uq(struct adf_accel_dev *accel_dev)
{
	return adf_ring_queue_set_mode(accel_dev, ADF_RING_QUEUE_WQ);
}

int adf_ring_queue_get_cfg_mode(struct adf_accel_dev *accel_dev,
				enum adf_ring_queue_mode *config_mode)
{
	char config_val[ADF_CFG_MAX_VAL_LEN_IN_BYTES] = { };
	bool cfg_is_uq;
	int ret;

	ret = adf_cfg_get_param_value(accel_dev, ADF_GENERAL_SEC,
				      ADF_RING_QUEUE_MODE, config_val);
	if (ret)
		return ret;

	ret = kstrtobool(config_val, &cfg_is_uq);
	if (ret) {
		dev_warn(&GET_DEV(accel_dev), "Invalid config value for %s: %s\n",
			 ADF_RING_QUEUE_MODE, config_val);
		return ret;
	}

	*config_mode = cfg_is_uq ? ADF_RING_QUEUE_UQ : ADF_RING_QUEUE_WQ;

	return ret;
}

int adf_ring_queue_enable(struct adf_accel_dev *accel_dev)
{
	enum adf_ring_queue_mode ring_queue_mode;
	int ret;

	ret = adf_ring_queue_get_cfg_mode(accel_dev, &ring_queue_mode);
	if (ret)
		return ret;

	accel_dev->ring_queue_mode = ring_queue_mode;

	return 0;
}
