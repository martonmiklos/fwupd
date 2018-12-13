/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2017 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <string.h>

#include "fu-plugin-vfuncs.h"
#include "libflashrom.h"

#define SELFCHECK_TRUE 1

struct FuPluginData {
	gchar			*flashrom_fn;
	static struct flashrom_flashctx *flashctx = NULL;
	static struct flashrom_layout *layout = NULL;
	static struct flashrom_programmer *flashprog = NULL;
};

void
fu_plugin_init (FuPlugin *plugin)
{
	fu_plugin_alloc_data (plugin, sizeof (FuPluginData));
}

void
fu_plugin_destroy (FuPlugin *plugin)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	g_free (data->flashrom_fn);
	flashrom_layout_release(data->layout);
	flashrom_programmer_shutdown(data->flashprog);
	flashrom_flash_release(data->flashctx);
}

gboolean
fu_plugin_startup (FuPlugin *plugin, GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	GPtrArray *hwids;
	g_autoptr(GError) error_local = NULL;

	/* we need flashrom from the host system */
	data->flashrom_fn = fu_common_find_program_in_path ("flashrom", &error_local);
	if (flashrom_init(SELFCHECK_TRUE)) {
		g_set_error (error,
			FWUPD_ERROR,
			FWUPD_ERROR_NOT_SUPPORTED,
			"Flashrom initialization error");
		return FALSE;
	}

	/* TODO: callback_implementation */
	//flashrom_set_log_callback((flashrom_log_callback *)&flashrom_print_cb);

	/* search for devices */
	hwids = fu_plugin_get_hwids (plugin);
	for (guint i = 0; i < hwids->len; i++) {
		const gchar *guid = g_ptr_array_index (hwids, i);
		const gchar *quirk_str;
		g_autofree gchar *quirk_key_prefixed = NULL;
		quirk_key_prefixed = g_strdup_printf ("HwId=%s", guid);
		quirk_str = fu_plugin_lookup_quirk_by_id (plugin,
							  quirk_key_prefixed,
							  "DeviceId");
		if (quirk_str != NULL) {
			g_autofree gchar *device_id = g_strdup_printf ("flashrom-%s", quirk_str);
			g_autoptr(FuDevice) dev = fu_device_new ();
			fu_device_set_id (dev, device_id);
			fu_device_set_quirks (dev, fu_plugin_get_quirks (plugin));
			fu_device_add_flag (dev, FWUPD_DEVICE_FLAG_INTERNAL);
			if (data->flashrom_fn != NULL) {
				fu_device_add_flag (dev, FWUPD_DEVICE_FLAG_UPDATABLE);
			} else {
				fu_device_set_update_error (dev, error_local->message);
			}
			fu_device_add_guid (dev, guid);
			fu_device_set_name (dev, fu_plugin_get_dmi_value (plugin, FU_HWIDS_KEY_PRODUCT_NAME));
			fu_device_set_vendor (dev, fu_plugin_get_dmi_value (plugin, FU_HWIDS_KEY_MANUFACTURER));
			fu_device_set_version (dev, fu_plugin_get_dmi_value (plugin, FU_HWIDS_KEY_BIOS_VERSION));
			fu_plugin_device_add (plugin, dev);
			fu_plugin_cache_add (plugin, device_id, dev);
			break;
		}
	}
	return TRUE;
}

static guint
fu_plugin_flashrom_parse_percentage (const gchar *lines_verbose)
{
	const guint64 addr_highest = 0x800000;
	guint64 addr_best = 0x0;
	g_auto(GStrv) chunks = NULL;

	/* parse 0x000000-0x000fff:S, 0x001000-0x001fff:S */
	chunks = g_strsplit_set (lines_verbose, "x-:S, \n\r", -1);
	for (guint i = 0; chunks[i] != NULL; i++) {
		guint64 addr_tmp;
		if (strlen (chunks[i]) != 6)
			continue;
		addr_tmp = g_ascii_strtoull (chunks[i], NULL, 16);
		if (addr_tmp > addr_best)
			addr_best = addr_tmp;
	}
	return (addr_best * 100) / addr_highest;
}

static void
fu_plugin_flashrom_read_cb (const gchar *line, gpointer user_data)
{
	FuDevice *device = FU_DEVICE (user_data);
	if (g_strcmp0 (line, "Reading flash...") == 0)
		fu_device_set_status (device, FWUPD_STATUS_DEVICE_VERIFY);
	fu_device_set_progress (device, fu_plugin_flashrom_parse_percentage (line));
}

static void
fu_plugin_flashrom_write_cb (const gchar *line, gpointer user_data)
{
	FuDevice *device = FU_DEVICE (user_data);
	if (g_strcmp0 (line, "Writing flash...") == 0)
		fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	fu_device_set_progress (device, fu_plugin_flashrom_parse_percentage (line));
}

gboolean
fu_plugin_update_prepare (FuPlugin *plugin,
			  FwupdInstallFlags flags,
			  FuDevice *device,
			  GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	g_autofree gchar *firmware_orig = NULL;
	g_autofree gchar *basename = NULL;
	g_autofree guint8 *newcontents = NULL;
	gsize flash_size = NULL;

	/* not us */
	if (fu_plugin_cache_lookup (plugin, fu_device_get_id (device)) == NULL)
		return TRUE;

	/* if the original firmware doesn't exist, grab it now */
	basename = g_strdup_printf ("flashrom-%s.bin", fu_device_get_id (device));
	firmware_orig = g_build_filename (LOCALSTATEDIR, "lib", "fwupd",
					  "builder", basename, NULL);
	if (!fu_common_mkdir_parent (firmware_orig, error))
		return FALSE;
	if (!g_file_test (firmware_orig, G_FILE_TEST_EXISTS)) {
		if (flashrom_programmer_init(&(data->flashprog), "internal", NULL)) {
			g_set_error (error,
				FWUPD_ERROR,
				FWUPD_ERROR_NOT_SUPPORTED,
				"Programmer initialization failed");
			return FALSE;
		}

		if (flashrom_flash_probe(&(data->flashctx), data->flashprog, NULL)) {
			g_set_error (error,
				FWUPD_ERROR,
				FWUPD_ERROR_NOT_SUPPORTED,
				"Flash probe failed");
			return FALSE;
		}

		flash_size = flashrom_flash_getsize(data->flashctx);
		newcontents = g_malloc(flash_size);

		// TODO: callback implementation
		if(flashrom_image_read(data->flashctx, newcontents, flash_size)) {
			g_set_error (error,
				FWUPD_ERROR,
				FWUPD_ERROR_READ,
				"Failed to get original firmware");
			return FALSE;
		}

		write_buf_to_file(newcontents, flash_size, firmware_orig);
	}

	return TRUE;
}

gboolean
fu_plugin_update (FuPlugin *plugin,
		  FuDevice *device,
		  GBytes *blob_fw,
		  FwupdInstallFlags flags,
		  GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	g_autofree gchar *firmware_fn = NULL;
	g_autofree gchar *tmpdir = NULL;
	g_autofree guint8 *newcontents = NULL;
	gsize flash_size = NULL;

	/* write blob to temp location */
	tmpdir = g_dir_make_tmp ("fwupd-XXXXXX", error);
	if (tmpdir == NULL)
		return FALSE;
	firmware_fn = g_build_filename (tmpdir, "flashrom-firmware.bin", NULL);
	if (!fu_common_set_contents_bytes (firmware_fn, blob_fw, error))
		return FALSE;

	if (flashrom_programmer_init(&(data->flashprog), "internal", NULL)) {
		g_set_error (error,
			FWUPD_ERROR,
			FWUPD_ERROR_NOT_SUPPORTED,
			"Programmer initialization failed");
		return FALSE;
	}

	if (flashrom_flash_probe(&(data->flashctx), data->flashprog, NULL)) {
		g_set_error (error,
			FWUPD_ERROR,
			FWUPD_ERROR_NOT_SUPPORTED,
			"Flash probe failed");
		return FALSE;
	}

	flashrom_flag_set(data->flashctx, FLASHROM_FLAG_VERIFY_AFTER_WRITE, TRUE);

	if (flashrom_layout_read_from_ifd(&(data->layout), data->flashctx, NULL, 0)) {
		g_set_error (error,
			FWUPD_ERROR,
			FWUPD_ERROR_READ,
			"Reading layout from Intel ICH descriptor failed");
		return FALSE;
	}

	/* Include bios region for safety reasons */
	if (flashrom_layout_include_region(data->layout, "bios")) {
		g_set_error (error,
			FWUPD_ERROR,
			FWUPD_ERROR_NOT_SUPPORTED,
			"Invalid region name");
		return FALSE;
	}

	flashrom_layout_set(data->flashctx, data->layout);

	flash_size = flashrom_flash_getsize(data->flashctx);
	newcontents = g_malloc(flash_size);

	if (read_buf_from_file(newcontents, flash_size, firmware_fn)) {
		g_set_error (error,
			FWUPD_ERROR,
			FWUPD_ERROR_NOT_SUPPORTED,
			"Read buffer from file failed");
		return FALSE;
	}

	if (flashrom_image_write(data->flashctx, newcontents, flash_size, NULL)) {
		g_set_error (error,
			FWUPD_ERROR,
			FWUPD_ERROR_WRITE,
			"Image write failed");
		return FALSE;
	}

	/* delete temp location */
	if (!fu_common_rmtree (tmpdir, error))
		return FALSE;

	/* success */
	return TRUE;
}
