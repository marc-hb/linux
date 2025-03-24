// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 Intel Corporation */
#include "adf_accel_devices.h"
#include "adf_common_drv.h"
#include "adf_gen4_hw_csr_data.h"
#include "adf_gen4_hw_data.h"
#include "adf_gen4_pfvf.h"
#include "adf_gen4_config.h"
#include "adf_gen4_vf_mig.h"
#include "adf_gen6_shared.h"

/*
 * QAT GEN4 and GEN6 devices often differ in terms of supported features,
 * options and internal logic. However, some of the mechanisms and register
 * layout are shared between those two GENs. This file serves as an abstraction
 * layer that allows to use existing GEN4 implementations that is also
 * applicable to GEN6 without additional overhead and complexity.
 */
void adf_gen6_init_pf_pfvf_ops(struct adf_pfvf_ops *pfvf_ops)
{
	adf_gen4_init_pf_pfvf_ops(pfvf_ops);
}
EXPORT_SYMBOL_GPL(adf_gen6_init_pf_pfvf_ops);

void adf_gen6_init_hw_csr_ops(struct adf_hw_csr_ops *csr_ops)
{
	return adf_gen4_init_hw_csr_ops(csr_ops);
}
EXPORT_SYMBOL_GPL(adf_gen6_init_hw_csr_ops);

int adf_gen6_dev_config(struct adf_accel_dev *accel_dev)
{
	return adf_gen4_dev_config(accel_dev);
}
EXPORT_SYMBOL_GPL(adf_gen6_dev_config);

int adf_gen6_cfg_dev_init(struct adf_accel_dev *accel_dev)
{
	return adf_gen4_cfg_dev_init(accel_dev);
}
EXPORT_SYMBOL_GPL(adf_gen6_cfg_dev_init);

void adf_gen6_init_vf_mig_ops(struct qat_migdev_ops *vfmig_ops)
{
	adf_gen4_init_vf_mig_ops(vfmig_ops);
}
EXPORT_SYMBOL_GPL(adf_gen6_init_vf_mig_ops);

int adf_gen6_get_ring_base_addr(struct adf_accel_dev *accel_dev,
				resource_size_t *base_addr, u32 ring_number,
				enum adf_ring_queue_mode queue_mode)
{
	return adf_gen4_get_ring_base_addr(accel_dev, base_addr, ring_number,
					   queue_mode);
}
EXPORT_SYMBOL_GPL(adf_gen6_get_ring_base_addr);
