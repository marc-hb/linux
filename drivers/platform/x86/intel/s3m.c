// SPDX-License-Identifier: GPL-2.0
/*
 * Intel S3M Bridge driver
 *
 * Copyright (c) 2024, Intel Corporation.
 * All Rights Reserved.
 *
 * Author: "Will Skrydlak" <will.j.skrydlak@linux.intel.com>
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/intel_vsec.h>
#include <linux/ioctl.h>
#include <linux/iopoll.h>
#include <linux/kdev_t.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/linux/s3m_if.h>


#define S3M_IOCTL_NAME			"s3m"
#define S3M_REGISTER_SIZE		4

#define CMD_TYPE_S3M_PROTOCOL		0x0C
#define CMD_METADATA_SIZE		0x01
#define CMD_METADATA			0x000C8086
#define CMD_HDR_SIZE			0x02
#define CMD_DISCOVERY_REQ_SIZE		0x01
#define CMD_DISCOVERY_RESP_SIZE		0x04
#define CMD_DISCOVERY_CMD_OFFSET	0x00
#define CMD_DISCOVERY_SIZE_OFFSET	0x01
#define CMD_DISCOVERY_FEATURES_OFFSET	0x02
#define CMD_SOCKET_ID_RESP_SIZE		0x02
#define CMD_SOCKET_ID_OFFSET		0x01
#define UNSUPPORTED_SOCKET_ID_OFFSET	0x01
#define IOCTL_CMD_METADATA_SIZE		0x02

#define REG_CTRL_OFFSET			0x00
#define REG_STATUS_OFFSET		0x04
#define REG_RDATA_OFFSET		0x0C
#define REG_WDATA_OFFSET		0x08

#define CMD_CTRL_ABORT			BIT(0)
#define CMD_CTRL_EN_INT			BIT(1)
#define CMD_CTRL_EN_MB			BIT(29)
#define CMD_CTRL_MB_DATA_RDY		BIT(30)
#define CMD_CTRL_GO			BIT(31)

#define CMD_STATUS_BUSY			BIT(0)
#define CMD_STATUS_INT			BIT(1)
#define CMD_STATUS_ERROR		BIT(2)
#define CMD_STATUS_OBJ_RDY		BIT(30)

#define POLL_INTERVAL_US		100
#define POLL_TIMEOUT_US			52000

static DEFINE_XARRAY_ALLOC(s3m_array);

struct s3m_priv {
	struct miscdevice miscdev;
	struct mutex s3m_lock; /* Locks the device to force single transaction */
	u32 devid;
	void __iomem *mmio_mb;
	u32 mmio_mb_buffer_size; /* Buffer size in bytes */
};

static int s3m_poll_mb_enabled(const void __iomem *mmio_mb, u32 *cr)
{
	return readl_poll_timeout(mmio_mb + REG_CTRL_OFFSET, *cr,
				  *cr & CMD_CTRL_EN_MB, POLL_INTERVAL_US, POLL_TIMEOUT_US);
}

static int s3m_poll_mb_enabled_no_data(const void __iomem *mmio_mb, u32 *cr)
{
	return readl_poll_timeout(mmio_mb + REG_CTRL_OFFSET, *cr,
				  !(*cr & CMD_CTRL_MB_DATA_RDY) && (*cr & CMD_CTRL_EN_MB),
				  POLL_INTERVAL_US, POLL_TIMEOUT_US);
}

static int s3m_poll_data_ready_clear(const void __iomem *mmio_mb, u32 *cr)
{
	return readl_poll_timeout(mmio_mb + REG_CTRL_OFFSET, *cr,
				  !(*cr & CMD_CTRL_MB_DATA_RDY), POLL_INTERVAL_US,
				  POLL_TIMEOUT_US);
}

static int s3m_poll_data_ready(const void __iomem *mmio_mb, u32 *cr)
{
	return readl_poll_timeout(mmio_mb + REG_CTRL_OFFSET, *cr,
				  *cr & CMD_CTRL_MB_DATA_RDY, POLL_INTERVAL_US,
				  POLL_TIMEOUT_US);
}

static int s3m_poll_status_not_busy(const void __iomem *mmio_mb, u32 *sr)
{
	return readl_poll_timeout((mmio_mb + REG_STATUS_OFFSET), *sr,
				  !(*sr & CMD_STATUS_BUSY), POLL_INTERVAL_US,
				  POLL_TIMEOUT_US);
}

static int s3m_poll_status_clear(const void __iomem *mmio_mb, u32 *sr)
{
	return readl_poll_timeout(mmio_mb + REG_STATUS_OFFSET, *sr, !*sr,
				  POLL_INTERVAL_US, POLL_TIMEOUT_US);
}

static int s3m_poll_status_object_ready(const void __iomem *mmio_mb, u32 *sr)
{
	return readl_poll_timeout(mmio_mb + REG_STATUS_OFFSET, *sr,
				  *sr & CMD_STATUS_OBJ_RDY, POLL_INTERVAL_US,
				  POLL_TIMEOUT_US);
}

static int s3m_write_dword(void __iomem *mmio_mb, const u32 dw)
{
	u32 status_reg, ctrl_reg;
	int ret;

	ret = s3m_poll_mb_enabled_no_data(mmio_mb, &ctrl_reg);
	if (ret)
		return ret;

	ret = s3m_poll_status_clear(mmio_mb, &status_reg);
	if (ret)
		return ret;

	writel(dw, mmio_mb + REG_WDATA_OFFSET);
	ctrl_reg |= CMD_CTRL_MB_DATA_RDY;
	writel(ctrl_reg, mmio_mb + REG_CTRL_OFFSET);

	return s3m_poll_data_ready_clear(mmio_mb, &ctrl_reg);
}

static int s3m_read_dword(void __iomem *mmio_mb, u32 *dw)
{
	u32 status_reg, ctrl_reg;
	int ret;

	ret = s3m_poll_status_object_ready(mmio_mb, &status_reg);
	if (ret)
		return ret;

	ret = s3m_poll_data_ready(mmio_mb, &ctrl_reg);
	if (ret)
		return ret;

	(*dw) = readl(mmio_mb + REG_RDATA_OFFSET);
	ctrl_reg &= ~CMD_CTRL_MB_DATA_RDY;
	writel(ctrl_reg, mmio_mb + REG_CTRL_OFFSET);

	return 0;
}

static void s3m_send_go(void __iomem *mmio_mb)
{
	u32 ctrl_reg;

	ctrl_reg = readl(mmio_mb + REG_CTRL_OFFSET);
	ctrl_reg |= CMD_CTRL_GO;
	writel(ctrl_reg, mmio_mb + REG_CTRL_OFFSET);
}

static struct s3m_if_cmd *s3m_write_cmd(void __iomem *mmio_mb, const u32 mmio_mb_buffer_size,
					const struct s3m_if_cmd *req, struct s3m_if_cmd *resp)
{
	const u32 *payload = (u32 *)&req->payload;
	u32 status_reg,	ctrl_reg;
	u32 cmd_metadata;
	int i, ret;

	/*
	 * The mmio_mb_buffer_size returned during discovery is the mailbox buffer size in BYTES.
	 * The length values when interacting with the mailbox are in the context of DWORDs. Thus,
	 * in order to check if request length will fit within the mailbox buffer,
	 * mmio_mb_buffer_size, must be divided by sizeof(u32).
	 */
	if (req->req_len >= (mmio_mb_buffer_size / S3M_REGISTER_SIZE))
		return ERR_PTR(-EINVAL);

	ret = s3m_poll_mb_enabled(mmio_mb, &ctrl_reg);
	if (ret)
		return ERR_PTR(ret);

	ret = s3m_poll_status_not_busy(mmio_mb, &status_reg);
	if (ret)
		return ERR_PTR(ret);

	if (status_reg & CMD_STATUS_ERROR) {
		ctrl_reg = readl(mmio_mb + REG_CTRL_OFFSET);
		ctrl_reg |= CMD_CTRL_ABORT;
		writel(ctrl_reg, mmio_mb + REG_CTRL_OFFSET);
		return ERR_PTR(-EAGAIN);
	}

	ret = s3m_write_dword(mmio_mb, CMD_METADATA);
	if (ret)
		return ERR_PTR(ret);

	ret = s3m_write_dword(mmio_mb, req->req_len + CMD_HDR_SIZE);
	if (ret)
		return ERR_PTR(ret);

	for (i = 0; i < req->req_len; i++) {
		ret = s3m_write_dword(mmio_mb, payload[i]);
		if (ret)
			return ERR_PTR(ret);
	}

	s3m_send_go(mmio_mb);
	ret = s3m_poll_status_object_ready(mmio_mb, &status_reg);
	if (ret)
		return ERR_PTR(ret);

	ret = s3m_read_dword(mmio_mb, (u32 *)&cmd_metadata);
	if (ret)
		return ERR_PTR(ret);
	else if (cmd_metadata != CMD_METADATA)
		return ERR_PTR(-EREMOTEIO);

	ret = s3m_read_dword(mmio_mb, &resp->resp_len);
	if (ret)
		return ERR_PTR(ret);

	if (resp->resp_len - CMD_HDR_SIZE > S3M_MB_MAX_PAYLOAD)
		return ERR_PTR(-EMSGSIZE);

	resp->resp_len -= CMD_HDR_SIZE;
	for (i = 0; i < resp->resp_len && !ret; i++) {
		ret = s3m_read_dword(mmio_mb, &resp->payload[i]);
		if (ret)
			return ERR_PTR(ret);
	}

	return resp;
}

static long s3m_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct s3m_if_cmd *ioctl_cmd = (struct s3m_if_cmd *)arg;
	struct s3m_if_cmd *resp __free(kfree) = NULL;
	struct s3m_if_cmd *req __free(kfree) = NULL;
	const void __user *usr_ptr;
	struct s3m_priv *priv;
	u32 req_len, rsp_len;
	void *cmd_ret;
	size_t result;
	int ret;

	priv = container_of(file->private_data, struct s3m_priv, miscdev);
	if (!priv)
		return -ENODATA;

	if (cmd != S3M_IF_SEND_CMD)
		return -EOPNOTSUPP;

	switch (cmd) {
	case S3M_IF_SEND_CMD:
		usr_ptr = &ioctl_cmd->req_len;
		ret = copy_from_user(&req_len, usr_ptr, sizeof(u32));
		if (ret)
			return ret;

		usr_ptr = &ioctl_cmd->resp_len;
		ret = copy_from_user(&rsp_len, usr_ptr, sizeof(u32));
		if (ret)
			return ret;

		if (req_len > S3M_MB_MAX_PAYLOAD)
			return -EINVAL;

		result = size_add(req_len, IOCTL_CMD_METADATA_SIZE);
		if (result == SIZE_MAX)
			return -EOVERFLOW;

		req = kcalloc(result, sizeof(u32), GFP_KERNEL);
		if (!req)
			return -ENOMEM;

		result = size_add(S3M_MB_MAX_PAYLOAD, IOCTL_CMD_METADATA_SIZE);
		if (result == SIZE_MAX)
			return -EOVERFLOW;

		resp = kcalloc(result, sizeof(u32), GFP_KERNEL);
		if (!resp)
			return -ENOMEM;

		usr_ptr = ioctl_cmd;
		req = memdup_user(usr_ptr, (req_len + IOCTL_CMD_METADATA_SIZE) * sizeof(u32));
		if (IS_ERR(req))
			return PTR_ERR(req);

		scoped_guard(mutex, &priv->s3m_lock) {
			cmd_ret = s3m_write_cmd(priv->mmio_mb, priv->mmio_mb_buffer_size, req,
						resp);
		}

		if (IS_ERR(cmd_ret))
			return PTR_ERR(cmd_ret);

		if (resp->resp_len > rsp_len)
			return -ENOMEM;

		return copy_to_user((u32 *)arg, resp,
				   (resp->resp_len + IOCTL_CMD_METADATA_SIZE) * sizeof(u32));
		break;
	}
	return 0;
}

static const struct file_operations s3m_fops = {
	.owner	  = THIS_MODULE,
	.unlocked_ioctl = s3m_ioctl,
};

static void s3m_remove(struct auxiliary_device *auxdev)
{
	struct s3m_priv *priv;

	priv = auxiliary_get_drvdata(auxdev);
	if (!priv)
		return;

	misc_deregister(&priv->miscdev);
}

static void s3m_lock_destroy(void *data)
{
	struct mutex *lock = data;

	mutex_destroy(lock);
}

static void s3m_xarray_destroy(void *data)
{
	u32 *id = data;

	xa_erase(&s3m_array, *id);
}

static int s3m_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	struct intel_vsec_device *intel_vsec_dev = auxdev_to_ivdev(auxdev);
	struct s3m_priv *priv = devm_kzalloc(&auxdev->dev, sizeof(*priv), GFP_KERNEL);
	u32 resp_stack[CMD_DISCOVERY_RESP_SIZE + IOCTL_CMD_METADATA_SIZE];
	u32 req_stack[CMD_DISCOVERY_REQ_SIZE + IOCTL_CMD_METADATA_SIZE];
	struct s3m_if_cmd *resp = (struct s3m_if_cmd *)resp_stack;
	struct s3m_if_cmd *req = (struct s3m_if_cmd *)req_stack;
	void *cmd_ret;
	int ret;

	if (!req || !resp || !priv)
		return -ENOMEM;

	mutex_init(&priv->s3m_lock);
	ret = devm_add_action_or_reset(&auxdev->dev, s3m_lock_destroy, &priv->s3m_lock);
	if (ret)
		return ret;

	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	priv->miscdev.fops = &s3m_fops;

	priv->mmio_mb = devm_ioremap_resource(&auxdev->dev, intel_vsec_dev->resource);
	if (IS_ERR(priv->mmio_mb))
		return PTR_ERR(priv->mmio_mb);

	req->req_len = CMD_DISCOVERY_REQ_SIZE;
	req->payload[CMD_DISCOVERY_CMD_OFFSET] = S3M_MB_CMD_DISC;

	priv->mmio_mb_buffer_size = CMD_DISCOVERY_RESP_SIZE * sizeof(u32);

	/*
	 * This will send the mailbox discovery command. The response data
	 * contains the mailbox size and feature set for a given S3M.
	 */
	pr_debug("%s: Writing doe 1: size %u\n", __func__, priv->mmio_mb_buffer_size);
	cmd_ret = s3m_write_cmd(priv->mmio_mb, priv->mmio_mb_buffer_size, req, resp);
	if (IS_ERR(cmd_ret)) {
		pr_debug("%s: Error: Doe command failed %d\n", __func__, ret);
		return PTR_ERR(cmd_ret);
	}

	if (resp->resp_len < CMD_DISCOVERY_RESP_SIZE)
		return -EBADMSG;

	priv->mmio_mb_buffer_size = resp->payload[CMD_DISCOVERY_SIZE_OFFSET];

	req->payload[CMD_DISCOVERY_CMD_OFFSET] |= S3M_MB_CMD_SOCKET_ID;

	pr_debug("%s: Writing doe 2: size %u\n", __func__, priv->mmio_mb_buffer_size);
	cmd_ret = s3m_write_cmd(priv->mmio_mb, priv->mmio_mb_buffer_size, req, resp);
	if (IS_ERR(cmd_ret)) {
		pr_debug("%s: Error: Doe command failed %d\n", __func__, ret);
		return PTR_ERR(cmd_ret);
	}

	ret = xa_alloc(&s3m_array, &priv->devid, intel_vsec_dev, xa_limit_31b, GFP_KERNEL);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&auxdev->dev, s3m_xarray_destroy, &priv->devid);
	if (ret)
		return ret;
	/*
	 * Older versions of OOBMSM firmware do not support the GET_SOCKET_ID command.
	 * On multi-socket systems, there is no guarantee that the first device to probe will
	 * correspond to Socket 0.
	 */
	if (resp->resp_len == CMD_SOCKET_ID_RESP_SIZE) {
		priv->miscdev.name = devm_kasprintf(&auxdev->dev, GFP_KERNEL, "s3m%d",
						    S3M_SOCKET_ID(resp->payload
						    [CMD_SOCKET_ID_OFFSET]));
	} else {
		/*
		 * If GET_SOCKET_ID command is not supported, enumerations will start at s3m1.
		 * UNSUPPORTED_SOCKET_ID_OFFSET will ensure there is no s3m0 device enumerated.
		 * This change was made to address a regression with ocode firmware.
		 */
		priv->miscdev.name = devm_kasprintf(&auxdev->dev, GFP_KERNEL, "s3m%d",
						    priv->devid + UNSUPPORTED_SOCKET_ID_OFFSET);
	}

	if (!priv->miscdev.name)
		return -ENOMEM;

	ret = misc_register(&priv->miscdev);
	if (ret)
		return ret;

	auxiliary_set_drvdata(auxdev, priv);

	return ret;
}

static const struct auxiliary_device_id s3m_id_table[] = {
	{ .name = "intel_vsec.s3m" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, s3m_id_table);

static struct auxiliary_driver s3m_aux_driver = {
	.id_table = s3m_id_table,
	.remove   = s3m_remove,
	.probe	  = s3m_probe,
};

module_auxiliary_driver(s3m_aux_driver);

MODULE_AUTHOR("Will Skrydlak <will.j.skrydlak@linux.intel.com>");
MODULE_DESCRIPTION("Intel S3M bridge driver.");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("INTEL_VSEC");
