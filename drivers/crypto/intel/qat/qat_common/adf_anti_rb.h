/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2025 Intel Corporation */
#ifndef ADF_ANTI_RB_H_
#define ADF_ANTI_RB_H_

#include <linux/types.h>

#define GET_ANTI_RB_DATA(accel_dev) (&(accel_dev)->hw_device->anti_rb_data)

#define ADF_SVN_FAIL_STS	0x03
#define ADF_SVN_RETRY_STS	0x02
#define ADF_SVN_RETRY_MS	250
#define ADF_SVN_RETRY_MAX	60
#define ADF_SVN_STS_MASK	0xFF

enum anti_rb {
	ENFORCED_MIN_SVN,
	PERMANENT_MIN_SVN,
	ACTIVE_SVN,
};

struct adf_accel_dev;
struct pci_dev;

struct adf_anti_rb_hw_data {
	bool (*anti_rb_enabled)(struct adf_accel_dev *accel_dev);
	u32 svncheck_offset;
	u32 svncheck_retry;
	bool sysfs_added;
};

int adf_anti_rb_commit(struct adf_accel_dev *accel_dev);
int adf_anti_rb_query(struct adf_accel_dev *accel_dev, enum anti_rb, u8 *svn);
int adf_anti_rb_check(struct pci_dev *pdev);

#endif /* ADF_ANTI_RB_H_ */
