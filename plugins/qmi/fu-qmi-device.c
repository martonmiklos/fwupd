/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2017-2018 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "config.h"

#include <string.h>
#include <libqmi-glib.h>

#include "fu-qmi-device.h"

struct _FuQmiDevice
{
	FuUsbDevice		 parent_instance;
};

G_DEFINE_TYPE (FuQmiDevice, fu_qmi_device, FU_TYPE_USB_DEVICE)

static void
fu_qmi_device_to_string (FuDevice *device, GString *str)
{
//	FuQmiDevice *self = FU_QMI_DEVICE (device);
	g_string_append (str, "  DfuQmiDevice:\n");
//	g_string_append_printf (str, "    timeout:\t\t%" G_GUINT32_FORMAT "\n", self->dnload_timeout);
}

gboolean
fu_qmi_device_download (FuQmiDevice *self, GBytes *blob, GError **error)
{
	return TRUE;
}

typedef struct {
	GCancellable	*cancellable;
	GError		**error;
	gboolean	 ret;
	GMainLoop	*loop;
	QmiDevice	*device;
	QmiClient	*client;
} FuQmiDeviceHelper;

static void
fu_qmi_list_stored_images_cb (QmiClientDms *client, GAsyncResult *res)
{
	FuQmiDeviceHelper *helper = g_async_result_get_user_data (res);
	QmiMessageDmsListStoredImagesOutput *output;
	GError *error = NULL;
	GArray *stored_images_list;

	output = qmi_client_dms_list_stored_images_finish (client, res, &error);
	if (output == NULL) {
		g_error ("error: operation failed: %s\n", error->message);
		//operation_shutdown (FALSE);
		return;
	}

	if (!qmi_message_dms_list_stored_images_output_get_result (output, &error)) {
		g_error ("error: couldn't list stored images: %s\n", error->message);
		qmi_message_dms_list_stored_images_output_unref (output);
		//operation_shutdown (FALSE);
		return;
	}
	if (!qmi_message_dms_list_stored_images_output_get_list (output,
								 &stored_images_list,
								 &error)) {
		g_error ("error: get stored image list: %s\n", error->message);
	}


	/* success */
	g_warning ("got images list %u", stored_images_list->len);
	helper->ret = TRUE;
	g_main_loop_quit (helper->loop);
}

static void
fu_qmi_allocate_client_cb (QmiDevice *dev, GAsyncResult *res)
{
	FuQmiDeviceHelper *helper = g_async_result_get_user_data (res);
	GError *error = NULL;

	helper->client = qmi_device_allocate_client_finish (dev, res, &error);
	if (helper->client == NULL) {
		g_error ("error: couldn't create client for the DMS service: %s\n",
					error->message);
	}

	/* get all the stored images */
	qmi_client_dms_list_stored_images (QMI_CLIENT_DMS (helper->client),
					   NULL, /* unused */
					   10, /* timeout */
					   helper->cancellable,
					   (GAsyncReadyCallback) fu_qmi_list_stored_images_cb,
					   helper);
}

static void
fu_qmi_device_open_cb (QmiDevice *dev, GAsyncResult *res)
{
	FuQmiDeviceHelper *helper = g_async_result_get_user_data (res);
	GError *error = NULL;

	if (!qmi_device_open_finish (dev, res, &error)) {
		g_error ("error: couldn't open the QmiDevice: %s\n",
					error->message);
	}

	g_debug ("QMI Device at '%s' ready",
		 qmi_device_get_path_display (dev));

	/* create a client for the requested service */
	qmi_device_allocate_client (dev,
				    QMI_SERVICE_DMS,
				    QMI_CID_NONE,
				    10,
				    helper->cancellable,
				    (GAsyncReadyCallback) fu_qmi_allocate_client_cb,
				    helper);
}

static void
fu_qmi_device_new_cb (GObject *unused, GAsyncResult *res)
{
	GError *error = NULL;
	FuQmiDeviceHelper *helper = g_async_result_get_user_data (res);

	helper->device = qmi_device_new_finish (res, &error);
	if (helper->device == NULL) {
		g_error ("error: couldn't create QmiDevice: %s\n",
					error->message);
	}

	/* open the device */
	qmi_device_open (helper->device,
			 QMI_DEVICE_OPEN_FLAGS_NONE,
			 15,
			 helper->cancellable,
			 (GAsyncReadyCallback) fu_qmi_device_open_cb,
			 helper);
}

static gboolean
fu_qmi_device_open (FuUsbDevice *device, GError **error)
{
//	FuQmiDevice *self = FU_QMI_DEVICE (device);
//	GUsbDevice *usb_device = fu_usb_device_get_dev (device);

#if 0
	/* open device and clear status */
	if (!g_usb_device_claim_interface (usb_device, 0x00, /* HID */
					   G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					   error)) {
		g_prefix_error (error, "failed to claim HID interface: ");
		return FALSE;
	}
#endif

	g_autoptr(GFile) file = g_file_new_for_path ("/dev/cdc-wdm0");
	FuQmiDeviceHelper *helper;

	helper = g_new0 (FuQmiDeviceHelper, 1);
	helper->cancellable = g_cancellable_new ();
	helper->loop = g_main_loop_new (NULL, FALSE);

	/* create QmiDevice */
	qmi_device_new (file,
			helper->cancellable,
			(GAsyncReadyCallback) fu_qmi_device_new_cb,
			helper);
	g_main_loop_run (helper->loop);

	g_error ("MOO");
#if 0


/**
 * qmi_client_dms_get_manufacturer:
 * @self: a #QmiClientDms.
 * @unused: %NULL. This message doesn't have any input bundle.
 * @timeout: maximum time to wait for the method to complete, in seconds.
 * @cancellable: a #GCancellable or %NULL.
 * @callback: a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: user data to pass to @callback.
 *
 * Asynchronously sends a Get Manufacturer request to the device.
 *
 * When the operation is finished, @callback will be invoked in the thread-default main loop of the thread you are calling this method from.
 *
 * You can then call qmi_client_dms_get_manufacturer_finish() to get the result of the operation.
 *
 * Since: 1.0
 */
void qmi_client_dms_get_manufacturer (
	QmiClientDms *self,
	gpointer unused,
	guint timeout,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	gpointer user_data);

/**
 * qmi_client_dms_get_manufacturer_finish:
 * @self: a #QmiClientDms.
 * @res: the #GAsyncResult obtained from the #GAsyncReadyCallback passed to qmi_client_dms_get_manufacturer().
 * @error: Return location for error or %NULL.
 *
 * Finishes an async operation started with qmi_client_dms_get_manufacturer().
 *
 * Returns: a #QmiMessageDmsGetManufacturerOutput, or %NULL if @error is set. The returned value should be freed with qmi_message_dms_get_manufacturer_output_unref().
 *
 * Since: 1.0
 */
QmiMessageDmsGetManufacturerOutput *qmi_client_dms_get_manufacturer_finish (
	QmiClientDms *self,
	GAsyncResult *res,
	GError **error);

gboolean qmi_message_dms_get_manufacturer_output_get_manufacturer (
    QmiMessageDmsGetManufacturerOutput *self,
    const gchar **value_manufacturer,
    GError **error);


void qmi_client_dms_list_stored_images (
	QmiClientDms *self,
	gpointer unused,
	guint timeout,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	gpointer user_data);

QmiMessageDmsListStoredImagesOutput *qmi_client_dms_list_stored_images_finish (
	QmiClientDms *self,
	GAsyncResult *res,
	GError **error);

typedef struct _QmiMessageDmsListStoredImagesOutputListImage {
	QmiDmsFirmwareImageType type;
	guint8 maximum_images;
	guint8 index_of_running_image;
	GArray *sublist;
} QmiMessageDmsListStoredImagesOutputListImage;

typedef struct _QmiMessageDmsListStoredImagesOutputListImageSublistSublistElement {
	guint8 storage_index;
	guint8 failure_count;
	GArray *unique_id;
	gchar *build_id;
} QmiMessageDmsListStoredImagesOutputListImageSublistSublistElement;

#endif

	/* success */
	return TRUE;
}

static gboolean
fu_qmi_device_close (FuUsbDevice *device, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (device);

	/* we're done here */
	if (!g_usb_device_release_interface (usb_device, 0x00, /* HID */
					     G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					     error)) {
		g_prefix_error (error, "failed to release interface: ");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static void
fu_qmi_device_init (FuQmiDevice *self)
{
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_UPDATABLE);
}

static void
fu_qmi_device_class_init (FuQmiDeviceClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	FuUsbDeviceClass *klass_usb_device = FU_USB_DEVICE_CLASS (klass);
	klass_device->to_string = fu_qmi_device_to_string;
	klass_usb_device->open = fu_qmi_device_open;
	klass_usb_device->close = fu_qmi_device_close;
}

FuQmiDevice *
fu_qmi_device_new (FuUsbDevice *device)
{
	FuQmiDevice *self = g_object_new (FU_TYPE_QMI_DEVICE, NULL);
	fu_device_incorporate (FU_DEVICE (self), FU_DEVICE (device));
	return self;
}
