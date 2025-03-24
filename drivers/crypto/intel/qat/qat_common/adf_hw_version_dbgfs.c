// SPDX-License-Identifier: (GPL-2.0-only)
/* Copyright(c) 2024 Intel Corporation */

#include <linux/debugfs.h>
#include <linux/types.h>

#include "adf_accel_devices.h"
#include "adf_common_drv.h"
#include "adf_hw_version_dbgfs.h"

/*
 * PSDID register in config space
 */
#define ADF_PSID_OFFSET 0x2e
/*
 * 6 read required for VERSION
 */
#define ADF_PSID_NUM_READS_VERSION 6
#define ADF_PSID_NO_EMULATION 0x00
/*
 * String buffer size
 */
#define HW_VERSION_BUFFER_SIZE 36
/*
 * Ascii value of 'a'
 */
#define ASCII_A 0x61
/*
 * PSID index value
 */
#define PLATFORM_TYPE_INDEX 0
#define VERSION_INDEX 3

static DEFINE_MUTEX(hw_version_read_lock);

static int adf_hw_version_debug_show(struct seq_file *sfile, void *v)
{
	struct adf_accel_dev *accel_dev = sfile->private;
	struct pci_dev *pdev = accel_dev->accel_pci_dev.pci_dev;
	u8 psid_read_index = 0, year, week, rel_num, rev;
	char tempBuffer[HW_VERSION_BUFFER_SIZE];
	u16 reg_value;

	/*
	 * Reset tag in PSID register
	 */
	pci_write_config_word(pdev, ADF_PSID_OFFSET, 0);
	do {
		pci_read_config_word(pdev, ADF_PSID_OFFSET, &reg_value);
		/*
		 * Decode emulation model information
		 */
		switch (psid_read_index) {
		case PLATFORM_TYPE_INDEX:
			seq_printf(sfile, "Platform type: %s\n",
				   ((reg_value & 0x7f) == 0) ? "FPGA" : "VP");
			break;
		case VERSION_INDEX:
			memset(tempBuffer, '\0', HW_VERSION_BUFFER_SIZE);

			/*
			 * Decode release number and year
			 */
			year = reg_value & 0xff;
			rel_num = (u8)((reg_value >> 8) & 0xff);

			/*
			 * Decode week number and revision
			 */
			psid_read_index++;
			pci_read_config_word(pdev, ADF_PSID_OFFSET, &reg_value);
			week = (u8)((reg_value >> 8) & 0xff);
			rev = ASCII_A + (reg_value & 0xff);

			seq_printf(sfile, "Release number: %d\n", rel_num);

			snprintf(tempBuffer, sizeof(tempBuffer), "%dww%d%c",
				 year, week, rev);
			seq_printf(sfile, "Release version: %s\n", tempBuffer);
			break;
		}
		psid_read_index++;
	} while (psid_read_index < ADF_PSID_NUM_READS_VERSION);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(adf_hw_version_debug);

void adf_hw_version_dbgfs_add(struct adf_accel_dev *accel_dev)
{
	struct pci_dev *pdev = accel_dev->accel_pci_dev.pci_dev;
	u16 reg_value = 0;

	/*
	 * Reset tag in PSID register
	 */
	pci_write_config_word(pdev, ADF_PSID_OFFSET, 0);
	pci_read_config_word(pdev, ADF_PSID_OFFSET, &reg_value);

	if (reg_value == ADF_PSID_NO_EMULATION)
		return;

	accel_dev->hw_version_dbgfile =
		debugfs_create_file("hw_version", 0400, accel_dev->debugfs_dir,
				    accel_dev, &adf_hw_version_debug_fops);
}

void adf_hw_version_dbgfs_rm(struct adf_accel_dev *accel_dev)
{
	debugfs_remove(accel_dev->hw_version_dbgfile);
	accel_dev->hw_version_dbgfile = NULL;
}
