library_name = 'gstlibuvch264src'

sources = [
  'gstlibuvch264src.c',
  'gstlibuvch264src.h',
  'gstlibuvch264src_internal.h',
  'uvc_device.c',
  'uvc_device.h',
  'frame_pipeline.c',
  'frame_pipeline.h',
  'ptz_control.c',
  'ptz_control.h',
  'spspps_cache.c',
  'spspps_cache.h',
  'gstlibuvch264src_error.c',
  'gstlibuvch264src_error.h',
]

shared_library(library_name, sources,
  dependencies: [gst_dep, gst_base_dep, libuvc_dep, libusb_dep],
  install: true,
  install_dir: join_paths(get_option('libdir'), 'gstreamer-1.0')
)
