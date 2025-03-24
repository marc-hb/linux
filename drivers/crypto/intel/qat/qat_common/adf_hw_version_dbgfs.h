/* SPDX-License-Identifier: (GPL-2.0-only) */
/* Copyright(c) 2024 Intel Corporation */
#ifndef ADF_HW_VERSION_DBGFS_H_
#define ADF_HW_VERSION_DBGFS_H_

struct adf_accel_dev;

void adf_hw_version_dbgfs_add(struct adf_accel_dev *accel_dev);
void adf_hw_version_dbgfs_rm(struct adf_accel_dev *accel_dev);

#endif /* ADF_HW_VERSION_DBGFS_H_ */
