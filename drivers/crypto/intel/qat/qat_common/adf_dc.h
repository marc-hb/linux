/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2025 Intel Corporation */
#ifndef ADF_DC_H
#define ADF_DC_H

struct adf_accel_dev;

void qat_comp_build_deflate(struct adf_accel_dev *accel_dev, void *ctx);
void qat_comp_build_zstd(struct adf_accel_dev *accel_dev, void *ctx);
void qat_comp_build_common(struct adf_accel_dev *accel_dev, void *ctx,
			   enum icp_qat_hw_compression_algo algo);

#endif /* ADF_DC_H */
