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
#include <linux/ioctl.h>
#include <linux/iopoll.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/intel_vsec.h>

#include <uapi/linux/s3m_if.h>

#define S3M_IOCTL_NAME	"s3m"

#define S3M_MAX_DEVICES		num_possible_cpus()

#define DOE_TYPE_S3M_PROTOCOL	0x0C
#define DOE_METADATA_SIZE	0x01
#define DOE_HDR_SIZE		0x02
#define DOE_DISCOVERY_REQ_SIZE	0x03
#define DOE_DISCOVERY_RESP_SIZE	0x05

#define REG_CTRL_OFFSET		0x00
#define REG_STATUS_OFFSET	0x04
#define REG_RDATA_OFFSET	0x0C
#define REG_WDATA_OFFSET	0x08

#define DOE_CTRL_ABORT		BIT(0)
#define DOE_CTRL_EN_INT		BIT(1)
#define DOE_CTRL_EN_MB		BIT(29)
#define DOE_CTRL_MB_DATA_RDY	BIT(30)
#define DOE_CTRL_GO		BIT(31)

#define DOE_STATUS_BUSY		BIT(0)
#define DOE_STATUS_INT		BIT(1)
#define DOE_STATUS_ERROR	BIT(2)
#define DOE_STATUS_OBJ_RDY	BIT(30)

#define POLL_INTERVAL_US	100
#define POLL_TIMEOUT_US		5200

static DEFINE_XARRAY_ALLOC(s3m_array);

struct s3m_priv {
	struct miscdevice miscdev;
	struct mutex s3m_lock; /* Locks the device to force single transaction */
	void __iomem *mmio_mb;
	u32 id;
	u32 mmio_mb_buffer_size; /* Buffer size in bytes */
	u32 s3m_features;
};

static int poll_mb_enabled(const void __iomem *mmio_mb, u32 *ctrl_reg)
{
	return readl_poll_timeout(mmio_mb + REG_CTRL_OFFSET, *ctrl_reg,
				  *ctrl_reg & DOE_CTRL_EN_MB, POLL_INTERVAL_US, POLL_TIMEOUT_US);
}

static int poll_mb_enabled_no_data(const void __iomem *mmio_mb, u32 *ctrl_reg)
{
	return readl_poll_timeout(mmio_mb + REG_CTRL_OFFSET, *ctrl_reg,
				  !(*ctrl_reg & DOE_CTRL_MB_DATA_RDY) && *ctrl_reg & DOE_CTRL_EN_MB,
				  POLL_INTERVAL_US, POLL_TIMEOUT_US);
}

static int poll_data_ready_clear(const void __iomem *mmio_mb, u32 *ctrl_reg)
{
	return readl_poll_timeout(mmio_mb + REG_CTRL_OFFSET, *ctrl_reg,
				  !(*ctrl_reg & DOE_CTRL_MB_DATA_RDY), POLL_INTERVAL_US,
				  POLL_TIMEOUT_US);
}

static int poll_data_ready(const void __iomem *mmio_mb, u32 *ctrl_reg)
{
	return readl_poll_timeout(mmio_mb + REG_CTRL_OFFSET, *ctrl_reg,
				  *ctrl_reg & DOE_CTRL_MB_DATA_RDY, POLL_INTERVAL_US,
				  POLL_TIMEOUT_US);
}

static int poll_status_not_busy(const void __iomem *mmio_mb, u32 *status_reg)
{
	return readl_poll_timeout((mmio_mb + REG_STATUS_OFFSET), *status_reg,
				  !(*status_reg & DOE_STATUS_BUSY), POLL_INTERVAL_US,
				  POLL_TIMEOUT_US);
}

static int poll_status_clear(const void __iomem *mmio_mb, u32 *status_reg)
{
	return readl_poll_timeout(mmio_mb + REG_STATUS_OFFSET, *status_reg, !*status_reg,
				  POLL_INTERVAL_US, POLL_TIMEOUT_US);
}

static int poll_status_object_ready(const void __iomem *mmio_mb, u32 *status_reg)
{
	return readl_poll_timeout(mmio_mb + REG_STATUS_OFFSET, *status_reg,
				  *status_reg & DOE_STATUS_OBJ_RDY, POLL_INTERVAL_US,
				  POLL_TIMEOUT_US);
}

static int write_dword(void __iomem *mmio_mb, const u32 dw)
{
	u32 status_reg, ctrl_reg;
	int ret;

	ret = poll_mb_enabled_no_data(mmio_mb, &ctrl_reg);
	if (ret)
		return ret;

	ret = poll_status_clear(mmio_mb, &status_reg);
	if (ret)
		return ret;

	writel(dw, mmio_mb + REG_WDATA_OFFSET);
	ctrl_reg |= DOE_CTRL_MB_DATA_RDY;
	writel(ctrl_reg, mmio_mb + REG_CTRL_OFFSET);

	return poll_data_ready_clear(mmio_mb, &ctrl_reg);
}

static int read_dword(void __iomem *mmio_mb, u32 *dw)
{
	u32 status_reg, ctrl_reg;
	int ret;

	ret = poll_status_object_ready(mmio_mb, &status_reg);
	if (ret)
		return ret;

	ret = poll_data_ready(mmio_mb, &ctrl_reg);
	if (ret)
		return ret;

	(*dw) = readl(mmio_mb + REG_RDATA_OFFSET);
	ctrl_reg &= ~DOE_CTRL_MB_DATA_RDY;
	writel(ctrl_reg, mmio_mb + REG_CTRL_OFFSET);

	return ret;
}

static void send_go(void __iomem *mmio_mb)
{
	u32 ctrl_reg;

	ctrl_reg = readl(mmio_mb + REG_CTRL_OFFSET);
	ctrl_reg |= DOE_CTRL_GO;
	writel(ctrl_reg, mmio_mb + REG_CTRL_OFFSET);
}

static int write_doe(void __iomem *mmio_mb, const u32 mmio_mb_buffer_size,
		     const struct s3m_if_cmd *req, struct s3m_if_cmd *resp)
{
	const u32 *payload = (u32 *)&req->vendor_id;
	u32 bound = mmio_mb_buffer_size / sizeof(u32);
	u32 payload_size = req->length;
	u32 status_reg,	ctrl_reg;
	u32 *resp_payload;
	int i, ret;

	if (req->length >= (mmio_mb_buffer_size / sizeof(u32)))
		return -EINVAL;

	ret = poll_mb_enabled(mmio_mb, &ctrl_reg);
	if (ret)
		return ret;

	ret = poll_status_not_busy(mmio_mb, &status_reg);
	if (ret)
		return ret;

	if (status_reg & DOE_STATUS_ERROR) {
		ctrl_reg = readl(mmio_mb + REG_CTRL_OFFSET);
		ctrl_reg |= DOE_CTRL_ABORT;
		writel(ctrl_reg, mmio_mb + REG_CTRL_OFFSET);
		return -EIO;
	}

	for (i = 0; i < payload_size && i < bound; ++i) {
		ret = write_dword(mmio_mb, payload[i]);
		if (ret)
			return ret;
	}

	send_go(mmio_mb);
	ret = poll_status_object_ready(mmio_mb, &status_reg);
	if (ret)
		return ret;

	ret = read_dword(mmio_mb, (u32 *)&resp->vendor_id);
	if (ret)
		return ret;

	ret = read_dword(mmio_mb, &resp->length);
	if (ret)
		return ret;

	if (resp->length - DOE_HDR_SIZE > S3M_MB_MAX_PAYLOAD)
		return -EMSGSIZE;

	resp_payload = resp->payload;
	for (i = 0; i < resp->length - DOE_HDR_SIZE && !ret; ++i)
		ret = read_dword(mmio_mb, &resp_payload[i]);

	return ret;
}

static long s3m_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct s3m_if_cmd *ioctl_cmd = (struct s3m_if_cmd *)arg;
	struct dentry *d_entry = file->f_path.dentry;
	struct s3m_if_cmd *resp __free(kfree) = NULL;
	struct s3m_if_cmd *req __free(kfree) = NULL;
	const char *filename, *ch_devid;
	struct s3m_priv *priv;
	u32 req_len, rsp_len;
	unsigned long devid;
	int ret;

	priv = container_of(file->private_data, struct s3m_priv, miscdev);
	if (!priv)
		return -EIO;

	guard(mutex)(&priv->s3m_lock);

	if (d_entry)
		filename = d_entry->d_name.name;
	else
		return -ENOENT;

	if (strncmp(S3M_IOCTL_NAME, filename, 3) != 0)
		return -EINVAL;

	ch_devid = (filename + 3);

	if (kstrtoul(ch_devid, 10, &devid) != 0)
		return -EINVAL;

	if (devid < 0 || devid >= S3M_MAX_DEVICES)
		return -ENFILE;

	if (cmd != S3M_IF_SEND_DOE)
		return -EOPNOTSUPP;

	switch (cmd) {
	case S3M_IF_SEND_DOE:
		ret = copy_from_user(&rsp_len,
			(void const __user *)&ioctl_cmd->total_len, sizeof(u32));
		if (ret)
			break;

		ret = copy_from_user(&req_len,
			(void const __user *)&ioctl_cmd->length, sizeof(u32));
		if (ret)
			break;

		if (req_len - DOE_HDR_SIZE > S3M_MB_MAX_PAYLOAD)
			return -EINVAL;

		req = kcalloc(req_len + DOE_METADATA_SIZE, sizeof(u32), GFP_KERNEL);
		if (!req)
			return -ENOMEM;

		resp = kcalloc(S3M_MB_MAX_PAYLOAD + DOE_HDR_SIZE, sizeof(u32), GFP_KERNEL);
		if (!resp)
			return -ENOMEM;

		ret = copy_from_user(req,
			(void const __user *)ioctl_cmd,
			(req_len + DOE_METADATA_SIZE) * sizeof(u32));
		if (ret)
			break;

		if (req->vendor_id != PCI_VENDOR_ID_INTEL)
			return -EINVAL;

		ret = write_doe(priv->mmio_mb, priv->mmio_mb_buffer_size, req, resp);
		if (ret)
			break;

		if (resp->length > rsp_len)
			return -ENOMEM;

		ret = copy_to_user((void __user *)arg, resp,
				   (resp->length + DOE_METADATA_SIZE) * sizeof(u32));
		break;
	}

	return ret;
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

	xa_erase(&s3m_array, priv->id);
	misc_deregister(&priv->miscdev);
}

static int s3m_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	struct intel_vsec_device *intel_vsec_dev = auxdev_to_ivdev(auxdev);
	struct s3m_if_cmd *resp __free(kfree) = NULL;
	struct s3m_if_cmd *req __free(kfree) = NULL;
	struct s3m_priv *priv;
	unsigned long needs;
	int ret;

	needs = BIT(OOBMSM_SUP_PLAT_INFO);

	ret = intel_vsec_suppliers_ready(intel_vsec_dev, needs);
	if (ret)
		return ret;

	priv = devm_kzalloc(&auxdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->s3m_lock);

	req = kcalloc(DOE_DISCOVERY_REQ_SIZE, sizeof(u32), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	resp = kcalloc(DOE_DISCOVERY_RESP_SIZE, sizeof(u32), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	priv->miscdev.fops = &s3m_fops;

	priv->mmio_mb = devm_ioremap_resource(&auxdev->dev, intel_vsec_dev->resource);
	if (!priv->mmio_mb)
		return PTR_ERR(priv->mmio_mb);

	req->vendor_id = PCI_VENDOR_ID_INTEL;
	req->doe_type = DOE_TYPE_S3M_PROTOCOL;
	req->length = DOE_DISCOVERY_REQ_SIZE;
	req->payload[0] = S3M_MB_CMD_DISC;

	priv->mmio_mb_buffer_size = DOE_DISCOVERY_RESP_SIZE * sizeof(u32);

	/*
	 * This will send the mailbox discovery command. The response data
	 * contains the size and feature set for a given S3M.
	 */
	pr_debug("s3m: Writing doe: size %d\n", priv->mmio_mb_buffer_size);
	ret = write_doe(priv->mmio_mb, priv->mmio_mb_buffer_size, req, resp);
	if (ret) {
		pr_debug("s3m: Error: Doe command failed %d\n", ret);
		return ret;
	}
	pr_debug("s3m: Command success\n");
	pr_debug("s3m: length %d\n", resp->length);

	if (resp->length < DOE_DISCOVERY_RESP_SIZE) {
		pr_debug("s3m: Error: Response shorter then %d\n", DOE_DISCOVERY_RESP_SIZE);
		return -EIO;
	}

	priv->mmio_mb_buffer_size = resp->payload[1];
	pr_debug("s3m: return buffer size %d\n", resp->payload[1]);
	priv->s3m_features = resp->payload[2];
	pr_debug("s3m: features 0x%x\n", resp->payload[2]);

	ret = xa_alloc(&s3m_array, &priv->id, intel_vsec_dev, xa_limit_31b, GFP_KERNEL);
	if (ret)
		return -ENOMEM;

	priv->miscdev.name = devm_kasprintf(&auxdev->dev, GFP_KERNEL, "s3m%d", priv->id);
	if (!priv->miscdev.name) {
		ret = -ENOMEM;
		goto erase_xarray;
	}

	ret = misc_register(&priv->miscdev);
	if (ret)
		goto erase_xarray;

	auxiliary_set_drvdata(auxdev, priv);

	ret = intel_oobmsm_set_supplier(NULL, intel_vsec_dev, OOBMSM_SUP_S3M_SIMICS);
	if (ret)
		goto unregister_misc;

	return 0;

unregister_misc:
	misc_deregister(&priv->miscdev);
erase_xarray:
	xa_erase(&s3m_array, priv->id);

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

static int __init s3m_driver_init(void)
{
	return auxiliary_driver_register(&s3m_aux_driver);
}

static void __exit s3m_driver_exit(void)
{
	auxiliary_driver_unregister(&s3m_aux_driver);
}
module_init(s3m_driver_init);
module_exit(s3m_driver_exit);

MODULE_AUTHOR("Will Skrydlak <will.j.skrydlak@linux.intel.com>");
MODULE_DESCRIPTION("Intel S3M bridge driver.");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("INTEL_VSEC");
