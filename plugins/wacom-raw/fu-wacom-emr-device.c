/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>
#include <gio/gio.h>

#include "fu-chunk.h"
#include "fu-wacom-common.h"
#include "fu-wacom-emr-device.h"

struct _FuWacomEmrDevice {
	FuWacomDevice		 parent_instance;
};

G_DEFINE_TYPE (FuWacomEmrDevice, fu_wacom_emr_device, FU_TYPE_WACOM_DEVICE)

static gboolean
fu_wacom_emr_device_setup (FuDevice *device, GError **error)
{
	FuWacomEmrDevice *self = FU_WACOM_EMR_DEVICE (device);

	/* get firmware version */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_IS_BOOTLOADER)) {
		fu_device_set_version (device, "0.0");
	} else {
		guint16 fw_ver;
		guint8 data[19] = { 0x03, 0x0 };
		g_autofree gchar *version = NULL;
		if (!fu_wacom_device_get_feature (FU_WACOM_DEVICE (self),
						  data, sizeof(data), error))
			return FALSE;
		fw_ver = fu_common_read_uint16 (data + 11, G_LITTLE_ENDIAN);
		fu_device_remove_flag (device, FWUPD_DEVICE_FLAG_IS_BOOTLOADER);
		version = fu_common_version_from_uint16 (fw_ver, FU_VERSION_FORMAT_PAIR);
		fu_device_set_version (device, version);
	}

	/* success */
	return TRUE;
}

static gboolean
fu_wacom_emr_device_check_mpu_type (FuWacomEmrDevice *self, GError **error)
{
	guint8 rsp[RSP_SIZE];
	guint8 cmd[] = {
		FU_WACOM_DEVICE_BL_REPORT_ID_CMD,
		FU_WACOM_DEVICE_BL_CMD_GET_MPUTYPE,
		0x07,
	};
	if (!fu_wacom_device_cmd (FU_WACOM_DEVICE (self),
				  cmd, sizeof(cmd), rsp, sizeof(rsp), 0,
				  FU_WACOM_DEVICE_CMD_FLAG_NONE, error)) {
		g_prefix_error (error, "failed to get MPU type: ");
		return FALSE;
	}
	if (rsp[RTRN_RSP] != 0x2e) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_FAILED,
			     "MPU is not W9013 : 0x%x",
			     rsp[RTRN_RSP]);
		return FALSE;
	}
	return TRUE;
}

static guint8
fu_wacom_emr_device_calc_checksum (guint8 init1, guint8 init2,
				   const guint8 *buf, guint8 bufsz)
{
	guint8 sum = 0;
	sum += init1;
	sum += init2;
	for (guint i = 0; i < 4; i++)
		sum += buf[i];
	return ~sum + 1;
}

static gboolean
fu_wacom_emr_device_erase_datamem (FuWacomEmrDevice *self, GError **error)
{
	guint8 rsp[RSP_SIZE];
	guint8 cmd[] = {
		FU_WACOM_DEVICE_BL_REPORT_ID_CMD,
		FU_WACOM_DEVICE_BL_CMD_ERASE_DATAMEM,
		FU_WACOM_DEVICE_BL_CMD_ERASE_DATAMEM,	/* echo */
		0x00,					/* erased block */
		0xff,					/* checksum */
	};
	cmd[4] = fu_wacom_emr_device_calc_checksum (0x05, 0x07, cmd, 4);
	if (!fu_wacom_device_cmd (FU_WACOM_DEVICE (self),
				  cmd, sizeof(cmd), rsp, sizeof(rsp), 50,
				  FU_WACOM_DEVICE_CMD_FLAG_POLL_ON_WAITING, error)) {
		g_prefix_error (error, "failed to erase datamem: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_wacom_emr_device_erase_codemem (FuWacomEmrDevice *self,
				   guint8 idx,
				   guint8 block_nr,
				   GError **error)
{
	guint8 rsp[RSP_SIZE];
	guint8 cmd[] = {
		FU_WACOM_DEVICE_BL_REPORT_ID_CMD,
		FU_WACOM_DEVICE_BL_CMD_ERASE_FLASH,
		idx,			/* echo */
		block_nr,		/* erased block */
		0xff,			/* checksum */
	};
	cmd[4] = fu_wacom_emr_device_calc_checksum (0x05, 0x07, cmd, 4);
	if (!fu_wacom_device_cmd (FU_WACOM_DEVICE (self),
				  cmd, sizeof(cmd), rsp, sizeof(rsp), 50,
				  FU_WACOM_DEVICE_CMD_FLAG_POLL_ON_WAITING, error)) {
		g_prefix_error (error, "failed to erase codemem: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_wacom_emr_device_write_block (FuWacomEmrDevice *self,
				 guint32 idx,
				 guint32 address,
				 const guint8 *data,
				 guint16 datasz,
				 GError **error)
{
	guint blocksz = fu_wacom_device_get_block_sz (FU_WACOM_DEVICE (self));
	g_autofree guint8 *cmd = g_malloc0 (blocksz + 8 + 2);
	cmd[0] = FU_WACOM_DEVICE_BL_REPORT_ID_CMD;
	cmd[1] = FU_WACOM_DEVICE_BL_CMD_WRITE_FLASH;
	cmd[2] = (guint8) idx;	/* echo */

	/* address */
	fu_common_write_uint32 (cmd + 3, address, G_LITTLE_ENDIAN);

	/* size */
	if (datasz != blocksz) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_FAILED,
			     "block size 0x%x != 0x%x untested",
			     datasz, (guint) blocksz);
		return FALSE;
	}
	cmd[7] = datasz / 8;

	/* data */
	memcpy (cmd + 8, data, datasz);

	/* cmd and data checksums */
	cmd[8 + blocksz + 0] = fu_wacom_emr_device_calc_checksum (0x05, 0x4c, cmd, 8);
	cmd[8 + blocksz + 1] = fu_wacom_emr_device_calc_checksum (0x00, 0x00, data, datasz);

	/* set, no get */
	if (!fu_wacom_device_set_feature (FU_WACOM_DEVICE (self),
					  cmd, sizeof(cmd), error)) {
		g_prefix_error (error, "failed to write codemem: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_wacom_emr_device_write_firmware (FuDevice *device, GPtrArray *chunks, GError **error)
{
	FuWacomEmrDevice *self = FU_WACOM_EMR_DEVICE (device);
	guint8 idx = 0;

	/* check MPU type: FIXME, move? */
	if (!fu_wacom_emr_device_check_mpu_type (self, error))
		return FALSE;

	/* erase */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_ERASE);
	if (!fu_wacom_emr_device_erase_datamem (self, error))
		return FALSE;
	for (guint i = 127; i >= 8; i--) {
		if (!fu_wacom_emr_device_erase_codemem (self, idx++, i, error))
			return FALSE;
	}

	/* write */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	for (guint i = 0; i < chunks->len; i++) {
		FuChunk *chk = g_ptr_array_index (chunks, i);
		if (fu_wacom_common_block_is_empty (chk->data, chk->data_sz))
			continue;
		if (!fu_wacom_emr_device_write_block (self,
						      chk->idx,
						      chk->address,
						      chk->data,
						      chk->data_sz,
						      error))
			return FALSE;
		g_usleep (50);

		/* after every 3 SetFeatures, do the matching GetFeatures */
		if (((i + 1) % 3) == 0) {
			for (guint j = i - 2; j < i + 1; j++) {
				guint8 rsp[RSP_SIZE];
				rsp[0] = FU_WACOM_DEVICE_BL_REPORT_ID_RSP;
				do {
					if (!fu_wacom_device_get_feature (FU_WACOM_DEVICE (self),
									  rsp, sizeof(rsp), error)) {
						g_prefix_error (error, "failed to write codemem: ");
						return FALSE;
					}
					if ((rsp[RTRN_CMD] != FU_WACOM_DEVICE_BL_CMD_WRITE_FLASH ||
					     rsp[RTRN_ECH] != j) ||
					    (rsp[RTRN_RSP] != FU_WACOM_DEVICE_RC_WAITING &&
					     rsp[RTRN_RSP] != FU_WACOM_DEVICE_RC_OK)) {
						g_set_error (error,
							     G_IO_ERROR,
							     G_IO_ERROR_FAILED,
							     "addr: %x res:%x",
							     (guint)chk->address,
							     (guint)rsp[RTRN_RSP]);
						return FALSE;
					}
				} while (rsp[RTRN_CMD] == FU_WACOM_DEVICE_BL_CMD_WRITE_FLASH &&
					 rsp[RTRN_ECH] == j &&
					 rsp[RTRN_RSP] == FU_WACOM_DEVICE_RC_WAITING);
			}
		}

		fu_device_set_progress_full (device, (gsize) i, (gsize) chunks->len);
	}

	fu_device_set_progress (device, 100);
	return TRUE;
}

static void
fu_wacom_emr_device_init (FuWacomEmrDevice *self)
{
	fu_device_set_name (FU_DEVICE (self), "Embedded Wacom EMR Device");
}

static void
fu_wacom_emr_device_class_init (FuWacomEmrDeviceClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	FuWacomDeviceClass *klass_wac_device = FU_WACOM_DEVICE_CLASS (klass);
	klass_device->setup = fu_wacom_emr_device_setup;
	klass_wac_device->write_firmware = fu_wacom_emr_device_write_firmware;
}

FuWacomEmrDevice *
fu_wacom_emr_device_new (FuUdevDevice *device)
{
	FuWacomEmrDevice *self = g_object_new (FU_TYPE_WACOM_EMR_DEVICE, NULL);
	fu_device_incorporate (FU_DEVICE (self), FU_DEVICE (device));
	return self;
}
