/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <gio/gio.h>

#include "fu-wacom-common.h"

gboolean
fu_wacom_common_check_reply (const guint8 *cmd, const guint8 *rsp, GError **error)
{
	if (cmd[RTRN_CMD] != rsp[RTRN_CMD]) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_FAILED,
			     "cmd failed, expected 0x%02x, got 0x%02x",
			     cmd[RTRN_CMD], rsp[RTRN_CMD]);
		return FALSE;
	}
	if (cmd[RTRN_ECH] != rsp[RTRN_ECH]) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_FAILED,
			     "cmd failed, expected 0x%02x, got 0x%02x",
			     cmd[RTRN_ECH], rsp[RTRN_ECH]);
		return FALSE;
	}
	return TRUE;
}

gboolean
fu_wacom_common_rc_set_error (guint8 rc, GError **error)
{
	if (rc == FU_WACOM_DEVICE_RC_OK)
		return TRUE;
	if (rc == FU_WACOM_DEVICE_RC_BUSY) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_BUSY,
			     "device is busy");
		return FALSE;
	}
	if (rc == FU_WACOM_DEVICE_RC_MCUTYPE) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "MCU type does not match");
		return FALSE;
	}
	if (rc == FU_WACOM_DEVICE_RC_PID) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "PID does not match");
		return FALSE;
	}
	if (rc == FU_WACOM_DEVICE_RC_VERSION) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "version is older");
		return FALSE;
	}
	g_set_error (error,
		     G_IO_ERROR,
		     G_IO_ERROR_FAILED,
		     "unknown error 0x%02x", rc);
	return FALSE;
}

gboolean
fu_wacom_common_block_is_empty (const guint8 *data, guint16 datasz)
{
	for (guint16 i = 0; i < datasz; i++) {
		if (data[i] != 0xff)
			return FALSE;
	}
	return TRUE;
}
