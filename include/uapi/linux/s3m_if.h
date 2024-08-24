/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Intel S3M Bridge Interface: Interface specification for the Secure Startup Services Module (S3M).
 * Copyright (c) 2024, Intel Corporation.
 *
 * Author: Will Skrydlak <will.j.skrydlak@linux.intel.com>
 */

#ifndef __S3M_IF_H
#define __S3M_IF_H

#include <linux/types.h>

#define S3M_MB_CMD_DISC 0x01
#define S3M_MB_MAX_PAYLOAD 1022

/**
 * struct s3m_if_cmd - Define S3M command to be processed
 * @total_len:		The total size allocated for @payload (in DWORDs). Size must be large enough
 *			to contain the response from the S3M. If not enough space is allocated, the
 *			driver will return an error.
 * @vendor_id		Only valid vendor ID is 0x8086 (Intel)
 * @doe_type		Only valid DOE type is 0x0C (S3M Proxy Protocol)
 * @reserved		Must be zero
 * @length:		The DOE object length of the input (in DWORDs).
 * @payload:		Contains the command bytes to be processed by S3M
 *
 * This structure is used to pass commands and receive response messages to/from the S3M via IOCTL
 * S3M_IF_SEND_DOE
 *
 */
struct s3m_if_cmd {
	__u32 total_len;
	__u16 vendor_id;
	__u8 doe_type;
	__u8 reserved;
	__u32 length;
	__u32 payload[];
} __counted_by(total_len);

#define S3M_IF_MAGIC		0xFB
#define S3M_IF_SEND_DOE		_IOWR(S3M_IF_MAGIC, 0, struct s3m_if_cmd *)
#endif
