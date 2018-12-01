/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>

#include "fu-io-channel.h"
#include "fu-mm-device.h"

typedef enum {
	FU_MM_DEVICE_DETACH_KIND_UNKNOWN,
	FU_MM_DEVICE_DETACH_KIND_QFASTBOOT,
	FU_MM_DEVICE_DETACH_KIND_LAST
} FuMmDeviceDetachKind;

struct _FuMmDevice {
	FuDevice		 parent_instance;
	MMModem			*modem;
	FuMmDeviceDetachKind	 detach_kind;
	FuIOChannel		*io_channel;
	gchar			*device_port_at;
};

G_DEFINE_TYPE (FuMmDevice, fu_mm_device, FU_TYPE_DEVICE)

static const gchar *
fu_mm_device_detach_kind_to_string (FuMmDeviceDetachKind kind)
{
	if (kind == FU_MM_DEVICE_DETACH_KIND_QFASTBOOT)
		return "qfastboot";
	return NULL;
}

static void
fu_mm_device_to_string (FuDevice *device, GString *str)
{
	FuMmDevice *self = FU_MM_DEVICE (device);
	g_string_append (str, "  FuMmDevice:\n");
	g_string_append_printf (str, "    path:\t\t\t%s\n",
				mm_modem_get_path (self->modem));
	g_string_append_printf (str, "    at-port:\t\t\t%s\n",
				self->device_port_at);
	g_string_append_printf (str, "    detach-kind:\t\t%s\n",
				fu_mm_device_detach_kind_to_string (self->detach_kind));
}

static gboolean
fu_mm_device_probe (FuDevice *device, GError **error)
{
	FuMmDevice *self = FU_MM_DEVICE (device);
	MMModemPortInfo *ports = NULL;
	guint n_ports = 0;

#if MM_CHECK_VERSION(1,9,999)
	/* FIXME: https://gitlab.freedesktop.org/mobile-broadband/ModemManager/issues/97 */
	if (mm_modem_get_firmware_update_method_FIXME (self->modem) == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "modem does not support firmware updates");
		return FALSE;
	}
	self->detach_kind = FU_MM_DEVICE_DETACH_KIND_UNKNOWN; //FIXME
#else
	self->detach_kind = FU_MM_DEVICE_DETACH_KIND_QFASTBOOT;
#endif

	fu_device_set_physical_id (device, mm_modem_get_device (self->modem));
	fu_device_set_vendor (device, mm_modem_get_manufacturer (self->modem));
	fu_device_set_name (device, mm_modem_get_model (self->modem));
	fu_device_set_version (device, mm_modem_get_revision (self->modem));
	fu_device_set_physical_id (device, mm_modem_get_device (self->modem));
	fu_device_add_guid (device, mm_modem_get_device_identifier (self->modem));

	/* look for the AT port */
	if (!mm_modem_get_ports (self->modem, &ports, &n_ports)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "failed to get port information");
		return FALSE;
	}
	for (guint i = 0; i < n_ports; i++) {
		if (ports[i].type == MM_MODEM_PORT_TYPE_AT) {
			self->device_port_at = g_strdup_printf ("/dev/%s", ports[i].name);
			break;
		}
	}
	mm_modem_port_info_array_free (ports, n_ports);

	/* this is required for detaching */
	if (self->device_port_at == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "failed to find AT port");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_mm_device_cmd (FuMmDevice *self, const gchar *cmd, GError **error)
{
	const gchar *buf;
	gsize bufsz = 0;
	g_autoptr(GBytes) at_req  = NULL;
	g_autoptr(GBytes) at_res  = NULL;
	g_autofree gchar *cmd_cr = g_strdup_printf ("%s\r\n", cmd);

	/* command */
	at_req = g_bytes_new (cmd_cr, strlen (cmd_cr));
	if (g_getenv ("FWUPD_MODEM_MANAGER_VERBOSE") != NULL)
		fu_common_dump_bytes (G_LOG_DOMAIN, "writing", at_req);
	if (!fu_io_channel_write_bytes (self->io_channel, at_req, 1500,
					FU_IO_CHANNEL_FLAG_FLUSH_INPUT, error)) {
		g_prefix_error (error, "failed to write %s: ", cmd);
		return FALSE;
	}

	/* response */
	at_res = fu_io_channel_read_bytes (self->io_channel, -1, 1500,
					   FU_IO_CHANNEL_FLAG_SINGLE_SHOT, error);
	if (at_res == NULL) {
		g_prefix_error (error, "failed to read response for %s: ", cmd);
		return FALSE;
	}
	if (g_getenv ("FWUPD_MODEM_MANAGER_VERBOSE") != NULL)
		fu_common_dump_bytes (G_LOG_DOMAIN, "read", at_res);
	buf = g_bytes_get_data (at_res, &bufsz);
	if (bufsz < 6) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to read valid response for %s", cmd);
		return FALSE;
	}
	if (memcmp (buf, "\r\nOK\r\n", 6) != 0) {
		g_autofree gchar *tmp = g_strndup (buf + 2, bufsz - 4);
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to read valid response for %s: %s",
			     cmd, tmp);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_mm_device_io_open (FuMmDevice *self, GError **error)
{
	/* open device */
	self->io_channel = fu_io_channel_new_file (self->device_port_at, error);
	if (self->io_channel == NULL)
		return FALSE;

	/* success */
	return TRUE;
}

static gboolean
fu_mm_device_io_close (FuMmDevice *self, GError **error)
{
	if (!fu_io_channel_shutdown (self->io_channel, error))
		return FALSE;
	g_clear_object (&self->io_channel);
	return TRUE;
}

static gboolean
fu_mm_device_detach_qfastboot (FuDevice *device, GError **error)
{
	FuMmDevice *self = FU_MM_DEVICE (device);
	g_autoptr(FuDeviceLocker) locker  = NULL;

	/* boot to fastboot mode */
	locker = fu_device_locker_new_full (device,
					    (FuDeviceLockerFunc) fu_mm_device_io_open,
					    (FuDeviceLockerFunc) fu_mm_device_io_close,
					    error);
	if (locker == NULL)
		return FALSE;
	if (!fu_mm_device_cmd (self, "AT", error))
		return FALSE;
	if (!fu_mm_device_cmd (self, "AT+QFASTBOOT", error)) {
		g_prefix_error (error, "rebooting into fastboot not supported: ");
		return FALSE;
	}

	/* success */
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static gboolean
fu_mm_device_inhibit (FuMmDevice *self, GError **error)
{
#if MM_CHECK_VERSION(1,9,999)
	//FIXME: prevent NM from activating the modem
	//https://gitlab.freedesktop.org/mobile-broadband/ModemManager/issues/98
	if (!mm_modem_inhibit_sync (self->modem, NULL, error))
		return FALSE;
#else
	/* disable modem */
	if (!mm_modem_disable_sync (self->modem, NULL, error))
		return FALSE;
#endif

	/* success */
	return TRUE;
}

static gboolean
fu_mm_device_uninhibit (FuMmDevice *self, GError **error)
{
#if MM_CHECK_VERSION(1,9,999)
	//FIXME: allow NM from activating the modem
	if (!mm_modem_uninhibit_sync (self->modem, NULL, error))
		return FALSE;
#else
	/* enable modem */
	if (!mm_modem_enable_sync (self->modem, NULL, error))
		return FALSE;
#endif

	return TRUE;
}

static gboolean
fu_mm_device_detach (FuDevice *device, GError **error)
{
	FuMmDevice *self = FU_MM_DEVICE (device);
	g_autoptr(FuDeviceLocker) locker  = NULL;

	/* boot to fastboot mode */
	locker = fu_device_locker_new_full (device,
					    (FuDeviceLockerFunc) fu_mm_device_inhibit,
					    (FuDeviceLockerFunc) fu_mm_device_uninhibit,
					    error);
	if (locker == NULL)
		return FALSE;

	/* fastboot */
	if (self->detach_kind == FU_MM_DEVICE_DETACH_KIND_QFASTBOOT)
		return fu_mm_device_detach_qfastboot (device, error);

	/* shouldn't get here ideally */
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "modem does not support detach");
	return FALSE;
}

static void
fu_mm_device_init (FuMmDevice *self)
{
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
	fu_device_set_summary (FU_DEVICE (self), "Mobile broadband device");
	fu_device_add_icon (FU_DEVICE (self), "network-modem");
}

static void
fu_mm_device_finalize (GObject *object)
{
	FuMmDevice *self = FU_MM_DEVICE (object);
	g_object_unref (self->modem);
	g_free (self->device_port_at);
	G_OBJECT_CLASS (fu_mm_device_parent_class)->finalize (object);
}

static void
fu_mm_device_class_init (FuMmDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	object_class->finalize = fu_mm_device_finalize;
	klass_device->to_string = fu_mm_device_to_string;
	klass_device->probe = fu_mm_device_probe;
	klass_device->detach = fu_mm_device_detach;
}

FuMmDevice *
fu_mm_device_new (MMModem *modem)
{
	FuMmDevice *self = g_object_new (FU_TYPE_MM_DEVICE, NULL);
	self->modem = g_object_ref (modem);
	return self;
}
