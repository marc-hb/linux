/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2025 Intel Corporation */

#ifndef _ADF_UACCE_H_
#define _ADF_UACCE_H_

#include <linux/hashtable.h>
#include <linux/types.h>
#include <linux/uacce.h>

#define ADF_PASID_HASHTABLE_BITS	8

struct adf_accel_dev;
struct adf_uacce_bank_data;

struct adf_uacce_data {
	DECLARE_HASHTABLE(pasid_ht, ADF_PASID_HASHTABLE_BITS);
	struct uacce_device *uacce_dev;
	struct adf_uacce_bank_data *bank_data;
	u32 svc_bitmask;
};

bool adf_uacce_is_enabled(struct adf_accel_dev *accel_dev);
int adf_uacce_enable(struct adf_accel_dev *accel_dev);
void adf_uacce_disable(struct adf_accel_dev *accel_dev);

#endif /* _ADF_UACCE_H_ */
