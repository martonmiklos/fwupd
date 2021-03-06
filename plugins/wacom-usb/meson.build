cargs = ['-DG_LOG_DOMAIN="FuPluginWacomUsb"']

install_data(['wacom-usb.quirk'],
  install_dir: join_paths(datadir, 'fwupd', 'quirks.d')
)

shared_module('fu_plugin_wacom_usb',
  sources : [
    'fu-wac-common.c',
    'fu-wac-device.c',
    'fu-wac-firmware.c',
    'fu-wac-module.c',
    'fu-wac-module-bluetooth.c',
    'fu-wac-module-touch.c',
    'fu-plugin-wacom-usb.c',
  ],
  include_directories : [
    include_directories('../..'),
    include_directories('../dfu'),
    include_directories('../../src'),
    include_directories('../../libfwupd'),
  ],
  install : true,
  install_dir: plugin_dir,
  c_args : cargs,
  dependencies : [
    plugin_deps,
  ],
  link_with : [
    dfu,
  ],
)

if get_option('tests')
  testdatadir = join_paths(meson.current_source_dir(), 'tests')
  cargs += '-DTESTDATADIR="' + testdatadir + '"'
  e = executable(
    'wacom-usb-self-test',
    sources : [
      'fu-self-test.c',
      'fu-wac-common.c',
      'fu-wac-firmware.c',
    ],
    include_directories : [
      include_directories('..'),
      include_directories('../dfu'),
      include_directories('../..'),
      include_directories('../../libfwupd'),
      include_directories('../../src'),
    ],
    dependencies : [
      libxmlb,
      gio,
      gusb,
      gudev,
      libm,
    ],
    link_with : [
      dfu,
      fwupd,
      libfwupdprivate,
    ],
    c_args : cargs
  )
  test('wacom-usb-self-test', e)
endif
