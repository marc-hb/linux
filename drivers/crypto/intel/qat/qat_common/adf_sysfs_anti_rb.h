/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2024 Intel Corporation */
#ifndef ADF_SYSFS_ANTI_RB_H_
#define ADF_SYSFS_ANTI_RB_H_

struct adf_accel_dev;

void adf_sysfs_anti_rb_add(struct adf_accel_dev *accel_dev);
void adf_sysfs_anti_rb_rm(struct adf_accel_dev *accel_dev);

#endif /* ADF_SYSFS_ANTI_RB_H_ */
