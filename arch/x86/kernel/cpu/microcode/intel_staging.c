// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Note this staging code is completely *untested* as the correspondent
 * hardware implementation is ready yet. So at the moment the code may
 * just give some estimation of software complexity.
 */

#define pr_fmt(fmt) "microcode: " fmt
#include <linux/delay.h>
#include <linux/io.h>

#include "internal.h"

#define MBOX_REG_NUM		4
#define MBOX_REG_SIZE		sizeof(u32)

#define MBOX_CONTROL_OFFSET	0x0
#define MBOX_STATUS_OFFSET	0x4

#define MASK_MBOX_CTRL_ABORT	BIT(0)
#define MASK_MBOX_CTRL_GO	BIT(31)

#define MASK_MBOX_STATUS_BUSY	BIT(0)
#define MASK_MBOX_STATUS_ERROR	BIT(2)
#define MASK_MBOX_STATUS_READY	BIT(31)

#define MBOX_MAX_LOAD(imgsz)	imgsz * 2
#define MBOX_WAIT_TIMEOUT	10 * MSEC_PER_SEC

#define STAGING_OFFSET_END	0xffffffff

/**
 * struct ucode_staging - Staging status to control the protocol
 *
 * @base	: an MMIO physical address of the first register
 * @img_addr	: a pointer to the payload image
 * @img_offset	: the next offset in the payload image
 * @img_size	: the payload size
 * @load_len	: the size of the mailbox object being loaded
 */
struct ucode_staging {
	void __iomem	*base;
	void		*img_addr;
	u32		img_offset;
	u32		img_size;
	u32		load_len;
};

static void mbox_load_chunk(struct ucode_staging *stg)
{
	pr_debug_once("Need to implement Staging mailbox loading code.\n");
}

static enum ucode_state mbox_wait_xaction(struct ucode_staging *stg)
{
	u32 timeout, status;

	for (timeout = 0; timeout < MBOX_WAIT_TIMEOUT; timeout++) {
		msleep(1);
		status = readl(stg->base + MBOX_STATUS_OFFSET);
		if (!(status & MASK_MBOX_STATUS_BUSY))
			break;
	}

	status = readl(stg->base + MBOX_STATUS_OFFSET);

	if (status & MASK_MBOX_STATUS_BUSY)
		return UCODE_TIMEOUT;

	if ((status & MASK_MBOX_STATUS_ERROR) || !(status & MASK_MBOX_STATUS_READY))
		return UCODE_ERROR;

	return UCODE_OK;
}

static enum ucode_state mbox_read_resp(struct ucode_staging *stg)
{
	pr_debug_once("Need to implement Staging response handler.\n");
	return UCODE_ERROR;
}

bool staging_work(u64 mmio_addr, void *payload, unsigned int size)
{
	struct ucode_staging stg = { .img_addr = payload, .img_size = size };
	u32 mbox_load = 0, max_load = MBOX_MAX_LOAD(stg.img_size);
	enum ucode_state state;

	stg.base = ioremap(mmio_addr, MBOX_REG_NUM * MBOX_REG_SIZE);

	writel(MASK_MBOX_CTRL_ABORT, stg.base + MBOX_CONTROL_OFFSET);
	while (stg.img_offset != STAGING_OFFSET_END) {
		mbox_load_chunk(&stg);

		/* Wait until the mbox data is processed. */
		state = mbox_wait_xaction(&stg);
		if (state != UCODE_OK)
			break;

		state = mbox_read_resp(&stg);
		if (state != UCODE_OK)
			break;

		mbox_load += stg.load_len;
		if (mbox_load > max_load) {
			state = UCODE_TIMEOUT;
			break;
		}
	}

	iounmap(stg.base);

	if (state != UCODE_OK) {
		pr_err("Staging failed with %s.\n", state == UCODE_TIMEOUT ? "timeout" : "error");
		return false;
	}

	return true;
}
