xorg_module = shared_module(
  'xeh',
  'xeh_xorg.c',
  include_directories: [protocol_inc, ipc_inc, registry_inc, objects_inc,
                        dispatch_inc, shm_inc, xorg_inc],
  c_args: [shm_define],
  dependencies: dependency('pixman-1'),
  link_with: [dispatch_lib, ipc_lib, registry_lib, objects_lib, protocol_lib,
              shm_lib],
  install: false,
)
