uvcsink_sources = [
  'gst_util.c',
  'log.c',
  'util.c',
  'uvc.c',
  'uvc_common.c',
  'uvc_connection.c',
  'uvc_connection_memfd.c',
  'uvc_connection_shm.c',
  'uvc_impl.c',
  'uvc_impl_gstreamer.c',
  'uvcsink.c',
  'uvcsink_event.c',
  'uvcsink_pad.c',
  'uvcsink_property.c',
  'uvcsink_query.c',
  'v4l2.c'
]

abi_incs = include_directories('ABI')
uvcsink = library('gstuvcsink',
  uvcsink_sources,
  c_args : gst_plugins_bad_args + ['-Wno-declaration-after-statement'] + ['-DMEMFD_SUPPORT_ENABLED'],
  include_directories : [configinc, abi_incs],
  dependencies : [gstbase_dep, dependency('threads')],
  install : true,
  install_dir : plugins_install_dir,
)
pkgconfig.generate(uvcsink, install_dir : plugins_pkgconfig_install_dir)
