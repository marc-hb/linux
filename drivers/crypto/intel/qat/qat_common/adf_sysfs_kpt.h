/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2025 Intel Corporation */
#ifndef ADF_SYSFS_KPT_H_
#define ADF_SYSFS_KPT_H_

struct adf_accel_dev;

int adf_sysfs_kpt_add(struct adf_accel_dev *accel_dev);
void adf_sysfs_kpt_rm(struct adf_accel_dev *accel_dev);

#endif /* ADF_SYSFS_KPT_H_ */
