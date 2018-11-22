Wacom RAW Support
=================

Introduction
------------

This plugin updates integrated Wacom AES and EMR devices. They are typically
connected using I²C and not USB.

GUID Generation
---------------

The HID DeviceInstanceId values are used, e.g. `HIDRAW\VEN_056A&DEV_4875`.

Quirk use
---------
This plugin uses the following plugin-specific quirks:

| Quirk                   | Description                         | Minimum fwupd version |
|-------------------------|-------------------------------------|-----------------------|
| `WacomI2cFlashBlockSize`| Block size to transfer firmware     | 1.2.2                 |
| `WacomI2cFlashBaseAddr` | Base address for firmware           | 1.2.2                 |
| `WacomI2cFlashSize`     | Maximum size of the firmware zone   | 1.2.2                 |
