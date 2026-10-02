project(
  'xeh',
  'c',
  version: '0.1.0',
  meson_version: '>= 0.61.0',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=true',
    'buildtype=debugoptimized',
  ],
)

add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-D_FORTIFY_SOURCE=2',
  '-fstack-protector-strong',
  language: 'c',
)

protocol_inc = include_directories('include', 'server/protocol')
ipc_inc = include_directories('server/ipc')
registry_inc = include_directories('server/registry')
objects_inc = include_directories('server/objects')
shm_inc = include_directories('server/shm')
graphics_inc = include_directories('server/graphics')
shm_define = '-DXEH_ENABLE_SHM=0'
if get_option('shm')
  shm_define = '-DXEH_ENABLE_SHM=1'
endif
dispatch_inc = include_directories('server/dispatch')

protocol_lib = static_library(
  'xeh-protocol',
  'server/protocol/protocol.c',
  include_directories: protocol_inc,
  install: false,
)

ipc_lib = static_library(
  'xeh-ipc',
  'server/ipc/ipc.c',
  'server/ipc/socket.c',
  include_directories: [protocol_inc, ipc_inc],
  link_with: protocol_lib,
  install: false,
)

registry_lib = static_library(
  'xeh-registry',
  'server/registry/registry.c',
  include_directories: [protocol_inc, registry_inc],
  link_with: protocol_lib,
  install: false,
)

objects_lib = static_library(
  'xeh-objects',
  'server/objects/object.c',
  include_directories: [protocol_inc, objects_inc],
  link_with: protocol_lib,
  install: false,
)

shm_lib = static_library(
  'xeh-shm',
  'server/shm/shm.c',
  include_directories: [protocol_inc, shm_inc],
  install: false,
)

graphics_lib = static_library(
  'xeh-graphics',
  'server/graphics/graphics.c',
  include_directories: [protocol_inc, shm_inc, graphics_inc],
  link_with: shm_lib,
  install: false,
)

dispatch_lib = static_library(
  'xeh-dispatch',
  'server/dispatch/dispatch.c',
  'server/dispatch/requests.c',
  'server/dispatch/events.c',
  include_directories: [protocol_inc, ipc_inc, registry_inc, objects_inc, dispatch_inc],
  link_with: [ipc_lib, registry_lib, objects_lib, protocol_lib],
  install: false,
)

client_inc = include_directories('libxeh', 'libxeh/include')
xeh_client_lib = shared_library(
  'xeh',
  'libxeh/connection.c',
  'libxeh/dispatch.c',
  'libxeh/shm.c',
  include_directories: [client_inc, protocol_inc, ipc_inc, shm_inc],
  c_args: [shm_define],
  link_with: [ipc_lib, protocol_lib, shm_lib],
  link_args: ['-Wl,--version-script=' + meson.current_source_dir() / 'libxeh/abi.map'],
  link_depends: 'libxeh/abi.map',
  version: meson.project_version(),
  soversion: '0',
  install: true,
)
install_headers('libxeh/include/xeh.h', subdir: 'xeh')
install_headers('include/xehproto.h', subdir: 'xeh')
import('pkgconfig').generate(
  xeh_client_lib,
  name: 'libxeh',
  description: 'XEH out-of-process extension client library',
  subdirs: 'xeh',
)

if get_option('ximage').enabled() and not get_option('shm')
  error('The XImage adapter requires -Dshm=true')
endif
x11_dep = dependency('x11', required: get_option('ximage'))
ximage_enabled = get_option('shm') and x11_dep.found()
if ximage_enabled
  xeh_ximage_lib = shared_library(
    'xeh-ximage',
    'libxeh/ximage.c',
    include_directories: [client_inc, protocol_inc],
    dependencies: x11_dep,
    link_with: xeh_client_lib,
    link_args: ['-Wl,--version-script=' + meson.current_source_dir() / 'libxeh/ximage_abi.map'],
    link_depends: 'libxeh/ximage_abi.map',
    version: meson.project_version(),
    soversion: '0',
    install: true,
  )
  install_headers('libxeh/include/xeh_ximage.h', subdir: 'xeh')
  import('pkgconfig').generate(
    xeh_ximage_lib,
    name: 'libxeh-ximage',
    filebase: 'libxeh-ximage',
    description: 'XImage adapter for libxeh',
    subdirs: 'xeh',
    requires: ['libxeh', 'x11'],
  )
endif

xcb_dri2_dep = dependency('xcb-dri2', required: get_option('dri'))
xcb_dri3_dep = dependency('xcb-dri3', required: get_option('dri'))
dri_enabled = xcb_dri2_dep.found() and xcb_dri3_dep.found()
if dri_enabled
  xeh_dri_lib = shared_library(
    'xeh-dri',
    'libxeh/dri.c',
    include_directories: [client_inc, protocol_inc],
    dependencies: [dependency('xcb'), xcb_dri2_dep, xcb_dri3_dep],
    link_with: xeh_client_lib,
    link_args: ['-Wl,--version-script=' + meson.current_source_dir() / 'libxeh/dri_abi.map'],
    link_depends: 'libxeh/dri_abi.map',
    version: meson.project_version(),
    soversion: '0',
    install: true,
  )
  install_headers('libxeh/include/xeh_dri.h', subdir: 'xeh')
  import('pkgconfig').generate(
    xeh_dri_lib,
    name: 'libxeh-dri',
    filebase: 'libxeh-dri',
    description: 'XCB DRI2 and DRI3 adapters for libxeh',
    subdirs: 'xeh',
    requires: ['libxeh', 'xcb', 'xcb-dri2', 'xcb-dri3'],
  )
endif

hello_c = executable(
  'xeh-hello',
  'examples/hello-c/main.c',
  include_directories: [client_inc, protocol_inc],
  link_with: xeh_client_lib,
  install: false,
)

if get_option('python')
  python = import('python').find_installation('python3', required: true)
  python.install_sources(
    'python/xeh/__init__.py',
    'python/xeh/_native.py',
    'python/xeh/connection.py',
    'python/xeh/extension.py',
    'python/xeh/request.py',
    subdir: 'xeh',
  )
endif

if get_option('fuzz')
  cc = meson.get_compiler('c')
  fuzz_probe = '''
    int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size) {
      (void)data;
      (void)size;
      return 0;
    }
  '''
  if not cc.links(fuzz_probe, args: ['-fsanitize=fuzzer'], name: 'libFuzzer runtime')
    error('libFuzzer requires a compiler with -fsanitize=fuzzer (usually Clang)')
  endif
  executable(
    'xeh-protocol-fuzz',
    'fuzz/protocol_fuzz.c',
    'server/protocol/protocol.c',
    'server/shm/shm.c',
    include_directories: [protocol_inc, shm_inc],
    c_args: ['-fsanitize=fuzzer'],
    link_args: ['-fsanitize=fuzzer'],
    install: false,
  )
endif

if get_option('xorg')
  xorg_sdk_path = get_option('xorg_sdk_include')
  xorg_inc = []
  if xorg_sdk_path != ''
    xorg_inc = [include_directories(xorg_sdk_path)]
  endif
  subdir('server/xorg')
  xcb_dep = dependency('xcb', required: false)
  if xcb_dep.found()
    executable(
      'xorg-smoke-test',
      'tests/xorg_smoke_test.c',
      include_directories: protocol_inc,
      dependencies: xcb_dep,
      link_with: protocol_lib,
      install: false,
    )
  endif
endif

if get_option('tests')
  protocol_test = executable(
    'protocol-test',
    'tests/protocol_test.c',
    include_directories: protocol_inc,
    link_with: protocol_lib,
    install: false,
  )

  test('protocol', protocol_test)

  fuzz_smoke_test = executable(
    'fuzz-smoke-test',
    'tests/fuzz_smoke_test.c',
    'fuzz/protocol_fuzz.c',
    include_directories: [protocol_inc, shm_inc],
    link_with: [protocol_lib, shm_lib],
    install: false,
  )
  test('fuzz-smoke', fuzz_smoke_test)

  ipc_test = executable(
    'ipc-test',
    'tests/ipc_test.c',
    include_directories: [protocol_inc, ipc_inc],
    link_with: [ipc_lib, protocol_lib],
    install: false,
  )

  test('ipc', ipc_test)

  registry_test = executable(
    'registry-test',
    'tests/registry_test.c',
    include_directories: [protocol_inc, ipc_inc, registry_inc],
    link_with: [registry_lib, ipc_lib, protocol_lib],
    install: false,
  )

  test('registry', registry_test)

  object_test = executable(
    'object-test',
    'tests/object_test.c',
    include_directories: [protocol_inc, objects_inc],
    link_with: [objects_lib, protocol_lib],
    install: false,
  )

  test('object', object_test)

  dispatch_test = executable(
    'dispatch-test',
    'tests/dispatch_test.c',
    include_directories: [protocol_inc, ipc_inc, registry_inc, objects_inc, dispatch_inc],
    link_with: [dispatch_lib, ipc_lib, registry_lib, objects_lib, protocol_lib],
    install: false,
  )

  test('dispatch', dispatch_test)

  client_test = executable(
    'libxeh-test',
    'tests/libxeh_test.c',
    include_directories: [client_inc, protocol_inc, ipc_inc],
    link_with: [xeh_client_lib, protocol_lib, ipc_lib],
    install: false,
  )
  test('libxeh', client_test, args: [hello_c.full_path()], timeout: 20)
  if get_option('shm')
    shm_test = executable(
      'shm-test',
      'tests/shm_test.c',
      include_directories: [client_inc, protocol_inc, ipc_inc, shm_inc],
      link_with: [xeh_client_lib, ipc_lib, protocol_lib, shm_lib],
      install: false,
    )
    test('shm', shm_test)
    graphics_test = executable(
      'graphics-test',
      'tests/graphics_test.c',
      include_directories: [protocol_inc, shm_inc, graphics_inc, objects_inc],
      link_with: [graphics_lib, shm_lib, objects_lib],
      install: false,
    )
    test('graphics', graphics_test)
  endif
  if ximage_enabled
    ximage_test = executable(
      'ximage-test',
      'tests/ximage_test.c',
      include_directories: [client_inc, protocol_inc],
      link_with: [xeh_ximage_lib, xeh_client_lib],
      install: false,
    )
    test('ximage', ximage_test)
  endif
  if dri_enabled
    dri_test = executable(
      'dri-test',
      'tests/dri_test.c',
      include_directories: [client_inc, protocol_inc],
      link_with: xeh_dri_lib,
      install: false,
    )
    test('dri', dri_test)
  endif
  if get_option('python')
    python_test_env = environment()
    python_test_env.set('PYTHONPATH', meson.current_source_dir() / 'python')
    python_test_env.set('XEH_LIBXEH_PATH', xeh_client_lib.full_path())
    test('python', python, args: [meson.current_source_dir() / 'tests/python_test.py'],
         env: python_test_env, timeout: 20)
  endif
endif
