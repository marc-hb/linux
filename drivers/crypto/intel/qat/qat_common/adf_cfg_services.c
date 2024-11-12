// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation */

#include <linux/export.h>
#include <linux/pci.h>
#include <linux/string.h>
#include "adf_cfg.h"
#include "adf_cfg_services.h"
#include "adf_cfg_strings.h"

const char *const adf_cfg_services[] = {
	[SVC_ID_SYM] = ADF_CFG_SYM,
	[SVC_ID_ASYM] = ADF_CFG_ASYM,
	[SVC_ID_DC] = ADF_CFG_DC,
	[SVC_ID_DCC] = ADF_CFG_DCC,
	[SVC_ID_DECOMP] = ADF_CFG_DECOMP,
};
EXPORT_SYMBOL_GPL(adf_cfg_services);

int adf_service_string_to_mask(struct adf_accel_dev *accel_dev, const char *buf,
			       size_t count, u32 *out_mask)
{
	struct adf_hw_device_data *hw_data = GET_HW_DATA(accel_dev);
	char services[ADF_CFG_MAX_VAL_LEN_IN_BYTES] = {'\0'};
	char *substr, *token;
	int id, num_svc = 0;
	u32 bit, mask = 0;

	if (count > ADF_CFG_MAX_VAL_LEN_IN_BYTES - 1)
		return -EINVAL;

	strscpy(services, buf, ADF_CFG_MAX_VAL_LEN_IN_BYTES);
	substr = services;
	token = strsep(&substr, ADF_SERVICES_DELIMITER);

	while (token && (num_svc < MAX_NUM_CONCURR_SVC)) {
		id = sysfs_match_string(adf_cfg_services, token);
		if (id < 0)
			return id;

		bit = BIT(id);
		if (mask & bit)
			return -EINVAL;

		mask |= bit;
		token = strsep(&substr, ADF_SERVICES_DELIMITER);
		num_svc++;
	}

	if (token || mask >= BIT(SVC_ID_COUNT))
		return -EINVAL;

	if (hw_data->service_supported && hw_data->service_supported(mask))
		return -EINVAL;

	*out_mask = mask;
	return 0;
}

int adf_service_mask_to_string(u32 in_mask, char *buf, size_t count)
{
	char str[ADF_CFG_MAX_STR_LEN] = {'\0'};
	unsigned long mask = in_mask;
	int len, i = 0;
	u32 bit;

	if (in_mask >= BIT(SVC_ID_COUNT))
		return -EINVAL;

	for_each_set_bit(bit, &mask, SVC_ID_COUNT) {
		if (i++) {
			strncat(str, ADF_SERVICES_DELIMITER,
				(ADF_CFG_MAX_STR_LEN - (strnlen(str,
				(ADF_CFG_MAX_STR_LEN - 1)) + 1)));
		}
		strncat(str, adf_cfg_services[bit],
			(ADF_CFG_MAX_STR_LEN - (strnlen(str, (ADF_CFG_MAX_STR_LEN - 1)) + 1)));
	}

	len = strnlen(str, ADF_CFG_MAX_STR_LEN);
	if (len > count)
		return -EOVERFLOW;

	strscpy(buf, str, count);

	return 0;
}

int adf_get_service_enabled(struct adf_accel_dev *accel_dev, u32 *mask)
{
	char services[ADF_CFG_MAX_VAL_LEN_IN_BYTES] = {0};
	int ret;

	ret = adf_cfg_get_param_value(accel_dev, ADF_GENERAL_SEC,
				      ADF_SERVICES_ENABLED, services);
	if (ret) {
		dev_err(&GET_DEV(accel_dev),
			ADF_SERVICES_ENABLED " param not found\n");
		return ret;
	}

	ret = adf_service_string_to_mask(accel_dev, services,
					 strnlen(services, ADF_CFG_MAX_VAL_LEN_IN_BYTES),
					 mask);
	if (ret)
		dev_err(&GET_DEV(accel_dev),
			"Invalid value of " ADF_SERVICES_ENABLED " param: %s\n",
			services);

	return ret;
}
EXPORT_SYMBOL_GPL(adf_get_service_enabled);
