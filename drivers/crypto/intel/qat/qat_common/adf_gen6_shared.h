/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2025 Intel Corporation */
#ifndef ADF_GEN6_SHARED_H_
#define ADF_GEN6_SHARED_H_

struct adf_pfvf_ops;
struct qat_migdev_ops;

void adf_gen6_init_pf_pfvf_ops(struct adf_pfvf_ops *pfvf_ops);
void adf_gen6_init_hw_csr_ops(struct adf_hw_csr_ops *csr_ops);
int adf_gen6_dev_config(struct adf_accel_dev *accel_dev);
int adf_gen6_cfg_dev_init(struct adf_accel_dev *accel_dev);
void adf_gen6_init_vf_mig_ops(struct qat_migdev_ops *vfmig_ops);
int adf_gen6_get_ring_base_addr(struct adf_accel_dev *accel_dev,
				resource_size_t *base_addr, u32 ring_number,
				enum adf_ring_queue_mode queue_mode);
void adf_gen6_ring_pasid_enable(void __iomem *csr_base_addr, u32 ring_number,
				bool at, bool adi, bool priv, u32 pasid);
void adf_gen6_ring_pasid_disable(void __iomem *csr_base_addr, u32 ring_number);
u32 adf_gen6_read_ring_pasid_value(void __iomem *csr_base_addr, u32 ring_number);

#endif/* ADF_GEN6_SHARED_H_ */
