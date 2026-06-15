# Hardware-independent ctest suites for the libuvch264src element.
#
#   plugin_load_smoke   registration/introspection only, against the REAL plugin.
#   mock_smoke          full READY->PLAYING streaming path, against a MOCK-backed
#                       copy of the plugin (tests/mock_libuvc.c stands in for
#                       libuvc). Optional -fsanitize=address / -fsanitize=thread
#                       variants are built when ENABLE_SANITIZERS=ON.

find_package(Threads REQUIRED)

# Directory holding GStreamer's own plugins (coreelements -> fakesink). Used to
# load just that one plugin explicitly in the streaming test, instead of scanning
# the whole system plugin path.
pkg_get_variable(GST_PLUGINS_DIR gstreamer-1.0 pluginsdir)

# -----------------------------------------------------------------------------
# Plugin-load smoke test (gst-check). Hardware-independent: it only exercises
# registration/introspection, never opening a UVC device.
# -----------------------------------------------------------------------------
add_executable(test_plugin_load test_plugin_load.c)
target_link_libraries(test_plugin_load PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
)

# The test dlopen()s the freshly built plugin via GST_PLUGIN_PATH, so it must be
# built first.
add_dependencies(test_plugin_load gstlibuvch264src)

# Isolate from any system-installed copy of the plugin and from the developer's
# real GStreamer registry, and make the freshly built libuvc resolvable when it
# was vendored at build time (see top-level CMakeLists.txt).
set(_smoke_env
  "GST_PLUGIN_PATH=${GST_PLUGIN_BUILD_DIR}"
  "GST_PLUGIN_SYSTEM_PATH="
  "GST_REGISTRY=${CMAKE_BINARY_DIR}/test-registry.bin"
  "GST_REGISTRY_FORK=no"
  "HOME=${CMAKE_BINARY_DIR}"
)
if(LIBUVC_RUNTIME_DIR)
  list(APPEND _smoke_env "LD_LIBRARY_PATH=${LIBUVC_RUNTIME_DIR}")
endif()

add_test(NAME plugin_load_smoke COMMAND test_plugin_load)
set_tests_properties(plugin_load_smoke PROPERTIES
  ENVIRONMENT "${_smoke_env}"
)

# -----------------------------------------------------------------------------
# SPS/PPS cache path-builder unit tests. Pure C against spspps_path.h (no GST,
# no device, no filesystem) - they assert M8 traversal blocking, M7 NULL-path
# safety, and the L5 codec+resolution cache key. One executable, two suites.
# -----------------------------------------------------------------------------
add_executable(test_cache test_cache.c)
target_include_directories(test_cache PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
)

add_test(NAME cache_path_safety COMMAND test_cache path)
add_test(NAME cache_resolution_key COMMAND test_cache key)

# -----------------------------------------------------------------------------
# Mock streaming harness.
#
# A second, mock-backed copy of the plugin links the element's own translation
# units against tests/mock_libuvc.c instead of the real libuvc. Because the mock
# provides every uvc_* symbol the element references, no real libuvc is linked.
# The smoke test drives this plugin to PLAYING; the mock feeder thread delivers
# crafted H.264 access units and we assert the element pops them.
# -----------------------------------------------------------------------------
option(ENABLE_SANITIZERS
  "Also build -fsanitize=thread / -fsanitize=address variants of the mock harness"
  OFF)

# The element sources, compiled verbatim into the mock-backed module.
# gstlibuvch264src_error.c is included because ptz_control.c posts bus errors via
# gst_libuvc_h264_src_post_error(); every mock-linked executable needs the symbol.
set(_element_srcs
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/gstlibuvch264src.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/uvc_device.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/frame_pipeline.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/ptz_control.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/spspps_cache.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/gstlibuvch264src_error.c
)

# add_mock_harness_variant(<variant> <sanitizer>)
#   <variant>   "" for the plain build, else a suffix ("asan", "tsan")
#   <sanitizer> "" or a -fsanitize=<name> value ("address", "thread")
function(add_mock_harness_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(plugin "gstlibuvch264src_mock${suffix}")
  set(testexe "test_mock_smoke${suffix}")
  set(ctestname "mock_smoke${suffix}")
  set(plugdir "${CMAKE_BINARY_DIR}/gstreamer-1.0-mock${suffix}")

  set(san_opts "")
  if(NOT sanitizer STREQUAL "")
    set(san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  endif()

  # --- mock-backed plugin module ---
  add_library(${plugin} MODULE ${_element_srcs} mock_libuvc.c)
  set_target_properties(${plugin} PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "gstlibuvch264src"
    LIBRARY_OUTPUT_DIRECTORY ${plugdir}
  )
  target_include_directories(${plugin} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${plugin} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::LIBUSB
    Threads::Threads
  )
  if(san_opts)
    target_compile_options(${plugin} PRIVATE ${san_opts})
    target_link_options(${plugin} PRIVATE ${san_opts})
  endif()

  # --- smoke test executable (loads the mock-backed plugin) ---
  add_executable(${testexe} test_mock_smoke.c)
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
  )
  add_dependencies(${testexe} ${plugin})
  if(san_opts)
    target_compile_options(${testexe} PRIVATE ${san_opts})
    target_link_options(${testexe} PRIVATE ${san_opts})
  endif()

  # Each variant gets its own plugin dir, registry, and HOME so they never
  # clobber each other (the smoke test writes the element's SPS/PPS cache under
  # HOME). The mock-backed plugin links no real libuvc, so no LD_LIBRARY_PATH.
  # Full isolation: the system plugin path is blanked so the registry never
  # scans unrelated third-party plugins (which trip the sanitizers). Only the
  # mock-backed plugin (GST_PLUGIN_PATH) plus an explicitly loaded coreelements
  # plugin (GST_COREELEMENTS_PLUGIN, for fakesink) are visible to the test.
  set(_home "${CMAKE_BINARY_DIR}/mock-home${suffix}")
  file(MAKE_DIRECTORY ${_home})
  set(_env
    "GST_PLUGIN_PATH=${plugdir}"
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/mock-registry${suffix}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_home}"
    "CK_FORK=no"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(sanitizer STREQUAL "address")
    # Leak detection is intentionally deferred: GStreamer/GLib's one-time global
    # allocations are noisy. ASan still catches buffer overflow, use-after-free,
    # and double-free - the high-value coverage the oversized-SPS path targets.
    list(APPEND _env "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  elseif(sanitizer STREQUAL "thread")
    # ignore_noninstrumented_modules makes TSan treat calls into uninstrumented
    # libraries (all of GStreamer/GLib here) atomically, so their internal mutex
    # bookkeeping is not misreported; the suppressions file is a documented
    # backstop. Races inside the instrumented plugin + mock are still reported.
    list(APPEND _env
      "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.suppressions:halt_on_error=1:ignore_noninstrumented_modules=1")
  endif()

  add_test(NAME ${ctestname} COMMAND ${testexe})
  set_tests_properties(${ctestname} PROPERTIES
    ENVIRONMENT "${_env}"
    # The element's control thread binds a fixed /tmp/libuvc_control socket, so
    # the mock variants must not run concurrently even under `ctest -j`.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )

  # --- lifecycle regression tests (unlock/unlock_stop + control_mutex) ---
  # Reuses the same mock-backed plugin and isolated env as the smoke test; a
  # per-test GST_CHECKS selects one case so each gets its own mock config.
  set(lifecycleexe "test_lifecycle${suffix}")
  add_executable(${lifecycleexe} test_lifecycle.c)
  target_link_libraries(${lifecycleexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
  )
  add_dependencies(${lifecycleexe} ${plugin})
  if(san_opts)
    target_compile_options(${lifecycleexe} PRIVATE ${san_opts})
    target_link_options(${lifecycleexe} PRIVATE ${san_opts})
  endif()

  # DISCONNECT mode (one frame, then silence) makes create() block, so the test
  # exercises unlock() interrupting it on teardown.
  add_test(NAME unlock_shutdown${suffix} COMMAND ${lifecycleexe})
  set_tests_properties(unlock_shutdown${suffix} PROPERTIES
    ENVIRONMENT "${_env};GST_CHECKS=test_unlock_shutdown;MOCK_UVC_FRAME_MODE=disconnect"
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )

  add_test(NAME mutex_restart${suffix} COMMAND ${lifecycleexe})
  set_tests_properties(mutex_restart${suffix} PROPERTIES
    ENVIRONMENT "${_env};GST_CHECKS=test_mutex_restart"
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endfunction()

add_mock_harness_variant("" "")
if(ENABLE_SANITIZERS)
  add_mock_harness_variant("asan" "address")
  add_mock_harness_variant("tsan" "thread")
endif()

# -----------------------------------------------------------------------------
# Device-selection unit tests (Task 6).
#
# The element TUs, the libuvc mock, and the driver are linked into ONE
# executable with the element type registered statically (no plugin .so). That
# keeps the mock's state in the test process, so the device-list leak counter
# (mock_uvc_device_lists_outstanding) is directly observable and device counts
# can be set programmatically. Each gst-check test is surfaced as its own ctest
# entry via GST_CHECKS so the suite can be filtered test-by-test.
# -----------------------------------------------------------------------------
add_executable(test_device_select
  test_device_select.c
  ${_element_srcs}
  mock_libuvc.c
)
target_include_directories(test_device_select PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_device_select PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  PkgConfig::LIBUSB
  Threads::Threads
)
if(ENABLE_SANITIZERS)
  target_compile_options(test_device_select PRIVATE
    -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(test_device_select PRIVATE -fsanitize=address)
endif()

# "<ctest name>:<gst-check test fn>" - one ctest entry each, selected by GST_CHECKS.
set(_device_select_cases
  "device_zero:test_device_zero"
  "index_validate:test_index_validate"
  "device_restart_leak:test_device_restart_leak"
  "device_selector:test_device_selector"
)
foreach(_case ${_device_select_cases})
  string(REPLACE ":" ";" _parts ${_case})
  list(GET _parts 0 _ctestname)
  list(GET _parts 1 _testfn)

  set(_ds_home "${CMAKE_BINARY_DIR}/device-select-home-${_ctestname}")
  file(MAKE_DIRECTORY ${_ds_home})

  set(_ds_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/device-select-registry-${_ctestname}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_ds_home}"
    "CK_FORK=no"
    "GST_CHECKS=${_testfn}"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(ENABLE_SANITIZERS)
    list(APPEND _ds_env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  add_test(NAME ${_ctestname} COMMAND test_device_select)
  set_tests_properties(${_ctestname} PROPERTIES
    ENVIRONMENT "${_ds_env}"
    # start() spawns the control thread which binds the fixed /tmp/libuvc_control
    # socket, so these must not run concurrently with each other or the mock
    # streaming variants.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endforeach()

# -----------------------------------------------------------------------------
# Native PTZ property/signal tests (Task 12).
#
# Same single-executable, statically-registered shape as test_device_select so
# the mock's last-written PTZ values are observable in-process
# (mock_uvc_get_last_pantilt/zoom). Each gst-check test is its own ctest entry
# selected via GST_CHECKS.
# -----------------------------------------------------------------------------
add_executable(test_ptz
  test_ptz.c
  ${_element_srcs}
  mock_libuvc.c
)
target_include_directories(test_ptz PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_ptz PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  PkgConfig::LIBUSB
  Threads::Threads
)
if(ENABLE_SANITIZERS)
  target_compile_options(test_ptz PRIVATE
    -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(test_ptz PRIVATE -fsanitize=address)
endif()

set(_ptz_cases
  "ptz_properties:test_ptz_properties"
  "ptz_capability_gate:test_ptz_capability_gate"
)
foreach(_case ${_ptz_cases})
  string(REPLACE ":" ";" _parts ${_case})
  list(GET _parts 0 _ctestname)
  list(GET _parts 1 _testfn)

  set(_ptz_home "${CMAKE_BINARY_DIR}/ptz-home-${_ctestname}")
  file(MAKE_DIRECTORY ${_ptz_home})

  set(_ptz_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/ptz-registry-${_ctestname}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_ptz_home}"
    "CK_FORK=no"
    "GST_CHECKS=${_testfn}"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(ENABLE_SANITIZERS)
    list(APPEND _ptz_env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  add_test(NAME ${_ctestname} COMMAND test_ptz)
  set_tests_properties(${_ctestname} PROPERTIES
    ENVIRONMENT "${_ptz_env}"
    # start() binds the fixed /tmp/libuvc_control socket via the control thread,
    # so the PTZ cases must not run concurrently with each other or the other
    # start()-driven suites under `ctest -j`.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endforeach()

# -----------------------------------------------------------------------------
# Opt-in hardened control-socket tests (Task 16).
#
# Same single-executable, statically-registered shape as test_ptz so start()/
# stop() run the REAL socket bind/unbind against the filesystem (only libuvc is
# mocked). No RESOURCE_LOCK is needed: each case binds a per-instance path under
# its own private $XDG_RUNTIME_DIR (created in the test), so nothing collides on
# the old fixed /tmp/libuvc_control path - which is gone now that the socket is
# opt-in and per-instance.
# -----------------------------------------------------------------------------
add_executable(test_socket
  test_socket.c
  ${_element_srcs}
  mock_libuvc.c
)
target_include_directories(test_socket PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_socket PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  PkgConfig::LIBUSB
  Threads::Threads
)
if(ENABLE_SANITIZERS)
  target_compile_options(test_socket PRIVATE
    -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(test_socket PRIVATE -fsanitize=address)
endif()

set(_socket_cases
  "socket_default_off:test_socket_default_off"
  "socket_hardened:test_socket_hardened"
)
foreach(_case ${_socket_cases})
  string(REPLACE ":" ";" _parts ${_case})
  list(GET _parts 0 _ctestname)
  list(GET _parts 1 _testfn)

  set(_socket_home "${CMAKE_BINARY_DIR}/socket-home-${_ctestname}")
  file(MAKE_DIRECTORY ${_socket_home})

  set(_socket_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/socket-registry-${_ctestname}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_socket_home}"
    "CK_FORK=no"
    "GST_CHECKS=${_testfn}"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(ENABLE_SANITIZERS)
    list(APPEND _socket_env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  add_test(NAME ${_ctestname} COMMAND test_socket)
  set_tests_properties(${_ctestname} PROPERTIES
    ENVIRONMENT "${_socket_env}"
    TIMEOUT 120
  )
endforeach()

# -----------------------------------------------------------------------------
# Error-mapping unit test (gst-check). Compiles the standalone error TU directly
# and asserts each uvc_error_t -> GST_ELEMENT_ERROR domain/code on the bus.
# No plugin and no UVC device are involved; only the helper + libuvc's error
# enum/strerror are linked.
# -----------------------------------------------------------------------------
add_executable(test_error_map
  test_error_map.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/gstlibuvch264src_error.c
)
target_include_directories(test_error_map PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_error_map PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  ${LIBUVC_LINK_TARGET}
)

# Isolate the GStreamer registry/HOME, and make a vendored libuvc resolvable at
# run time when it was built via FetchContent (see top-level CMakeLists.txt).
set(_errmap_env
  "GST_REGISTRY=${CMAKE_BINARY_DIR}/errmap-registry.bin"
  "GST_REGISTRY_FORK=no"
  "HOME=${CMAKE_BINARY_DIR}"
)
if(LIBUVC_RUNTIME_DIR)
  list(APPEND _errmap_env "LD_LIBRARY_PATH=${LIBUVC_RUNTIME_DIR}")
endif()

add_test(NAME error_map COMMAND test_error_map)
set_tests_properties(error_map PROPERTIES ENVIRONMENT "${_errmap_env}")

# -----------------------------------------------------------------------------
# SPS/PPS/VPS bounds-clamp regression (C1, heap overflow). Reuses the mock-backed
# plugin built by add_mock_harness_variant, but runs the feeder in oversized-SPS
# mode so an unclamped NAL copy would smash the element's fixed 1024 B SPS
# buffer. The ASAN variant is the teeth: an overflow aborts the run; the clamp
# keeps it green.
# -----------------------------------------------------------------------------
function(add_sps_bounds_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(plugin "gstlibuvch264src_mock${suffix}")   # built by add_mock_harness_variant
  set(testexe "test_sps_bounds${suffix}")
  set(ctestname "sps_bounds${suffix}")
  set(plugdir "${CMAKE_BINARY_DIR}/gstreamer-1.0-mock${suffix}")

  set(san_opts "")
  if(NOT sanitizer STREQUAL "")
    set(san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  endif()

  add_executable(${testexe} test_sps_bounds.c)
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
  )
  add_dependencies(${testexe} ${plugin})
  if(san_opts)
    target_compile_options(${testexe} PRIVATE ${san_opts})
    target_link_options(${testexe} PRIVATE ${san_opts})
  endif()

  set(_home "${CMAKE_BINARY_DIR}/bounds-home${suffix}")
  file(MAKE_DIRECTORY ${_home})
  set(_env
    "GST_PLUGIN_PATH=${plugdir}"
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/bounds-registry${suffix}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_home}"
    "CK_FORK=no"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
    "MOCK_UVC_FRAME_MODE=oversized_sps"
  )
  if(sanitizer STREQUAL "address")
    # always-malloc so each GObject instance is a plain malloc with an ASan
    # redzone right after it; otherwise the SPS overflow would land inside the
    # instance (intra-object) where ASan cannot see it. detect_leaks=0 keeps
    # GLib's one-time global allocations from drowning the real finding.
    list(APPEND _env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1"
      "G_SLICE=always-malloc")
  endif()

  add_test(NAME ${ctestname} COMMAND ${testexe})
  set_tests_properties(${ctestname} PROPERTIES
    ENVIRONMENT "${_env}"
    # Shares the element's fixed /tmp/libuvc_control socket with the other mock
    # variants, so it must not run concurrently with them under `ctest -j`.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endfunction()

add_sps_bounds_variant("" "")
if(ENABLE_SANITIZERS)
  add_sps_bounds_variant("asan" "address")
endif()

# -----------------------------------------------------------------------------
# Clock + PTS critical-section tests (Task 10).
#
# Statically links the element TUs, the libuvc mock, and the driver into one
# executable (like test_device_select) so the racing set_clock() vmethod and the
# mock feeder's frame_callback() run in the SAME instrumented binary - what TSan
# needs to observe both sides of the clock/PTS race. The error helper TU is
# included because ptz_control.c references it. The -fsanitize=thread variant is
# the gate; it uses a dedicated suppressions file that does NOT mask
# frame_callback()/set_clock(), so a lock regression reappears as a reported
# race instead of being silently baselined.
# -----------------------------------------------------------------------------
function(add_pts_safety_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_pts_thread_safety${suffix}")

  set(san_opts "")
  if(NOT sanitizer STREQUAL "")
    set(san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  endif()

  add_executable(${testexe}
    test_pts_thread_safety.c
    ${_element_srcs}
    ${CMAKE_SOURCE_DIR}/libuvch264src/src/gstlibuvch264src_error.c
    mock_libuvc.c
  )
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    PkgConfig::LIBUSB
    Threads::Threads
  )
  if(san_opts)
    target_compile_options(${testexe} PRIVATE ${san_opts})
    target_link_options(${testexe} PRIVATE ${san_opts})
  endif()

  # "<ctest name>:<gst-check test fn>" - one ctest entry each via GST_CHECKS.
  set(_pts_cases
    "pts_thread_safety${suffix}:test_pts_clock_race"
    "frame_throughput${suffix}:test_frame_throughput"
  )
  foreach(_case ${_pts_cases})
    string(REPLACE ":" ";" _parts ${_case})
    list(GET _parts 0 _ctestname)
    list(GET _parts 1 _testfn)

    set(_pts_home "${CMAKE_BINARY_DIR}/pts-home-${_ctestname}")
    file(MAKE_DIRECTORY ${_pts_home})

    set(_pts_env
      "GST_PLUGIN_SYSTEM_PATH="
      "GST_REGISTRY=${CMAKE_BINARY_DIR}/pts-registry-${_ctestname}.bin"
      "GST_REGISTRY_FORK=no"
      "HOME=${_pts_home}"
      "CK_FORK=no"
      "GST_CHECKS=${_testfn}"
      "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
    )
    if(sanitizer STREQUAL "thread")
      list(APPEND _pts_env
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan_pts.suppressions:halt_on_error=1:ignore_noninstrumented_modules=1")
    endif()

    add_test(NAME ${_ctestname} COMMAND ${testexe})
    set_tests_properties(${_ctestname} PROPERTIES
      ENVIRONMENT "${_pts_env}"
      # start() binds the fixed /tmp/libuvc_control socket, so these must not run
      # concurrently with each other or the other mock-backed variants.
      RESOURCE_LOCK uvc_control_socket
      TIMEOUT 120
    )
  endforeach()
endfunction()

add_pts_safety_variant("" "")
if(ENABLE_SANITIZERS)
  add_pts_safety_variant("tsan" "thread")
endif()

# -----------------------------------------------------------------------------
# USB teardown tests (Task 11, H1 + L8).
#
# Links the element TUs, the libuvc mock, AND a dedicated libusb mock into one
# executable - and deliberately does NOT link the real libusb, so mock_libusb.c
# is the sole provider of the libusb-1.0 symbols force_usb_release() calls.
# MOCK_LIBUSB_TEARDOWN switches the libuvc mock to hand back a real (mock) libusb
# handle and to model uvc_close() closing it, so the old double-close (the handle
# closed in force_usb_release() and again in uvc_close()) is observable. The ASAN
# variant aborts on that double-free; the fixed teardown stays clean and the test
# also asserts one close per open and that the interface count came from the
# active config descriptor, not a hardcoded 0..7 range.
# -----------------------------------------------------------------------------
function(add_usb_teardown_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_usb_teardown${suffix}")

  set(san_opts "")
  if(NOT sanitizer STREQUAL "")
    set(san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  endif()

  add_executable(${testexe}
    test_usb_teardown.c
    ${_element_srcs}
    mock_libuvc.c
    mock_libusb.c
  )
  target_compile_definitions(${testexe} PRIVATE MOCK_LIBUSB_TEARDOWN)
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
    ${LIBUSB_INCLUDE_DIRS}
  )
  # No PkgConfig::LIBUSB: mock_libusb.c supplies every libusb symbol, and linking
  # the real library too would multiply-define them.
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    Threads::Threads
  )
  if(san_opts)
    target_compile_options(${testexe} PRIVATE ${san_opts})
    target_link_options(${testexe} PRIVATE ${san_opts})
  endif()

  set(_home "${CMAKE_BINARY_DIR}/usb-teardown-home${suffix}")
  file(MAKE_DIRECTORY ${_home})
  set(_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/usb-teardown-registry${suffix}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_home}"
    "CK_FORK=no"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(sanitizer STREQUAL "address")
    list(APPEND _env "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  add_test(NAME usb_teardown${suffix} COMMAND ${testexe})
  set_tests_properties(usb_teardown${suffix} PROPERTIES
    ENVIRONMENT "${_env}"
    # start() binds the fixed /tmp/libuvc_control socket via the control thread,
    # so this must not run concurrently with the other start()-driven suites.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endfunction()

add_usb_teardown_variant("" "")
if(ENABLE_SANITIZERS)
  add_usb_teardown_variant("asan" "address")
endif()

# -----------------------------------------------------------------------------
# negotiate() tests (Task 13).
#
# Statically registers the element (like test_device_select) so the mock's
# format descriptor can be shaped in-process via mock_uvc_set_format_mode().
#   negotiate_edge_*   a zero-format device posts a RESOURCE bus error, and a
#                      0 fps / zero device-interval descriptor fails gracefully
#                      instead of taking SIGFPE on the framerate divisions (H4).
#   negotiate_leak     -fsanitize=address build only: N renegotiations leak no
#                      GstCaps (M3). LeakSanitizer's recoverable check is the
#                      gate; an __lsan_disable() warm-up window plus a
#                      suppressions file keep GStreamer's one-time init noise out
#                      of the result while the per-renegotiation caps leak trips.
# -----------------------------------------------------------------------------
function(add_negotiate_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_negotiate${suffix}")

  set(san_opts "")
  if(NOT sanitizer STREQUAL "")
    set(san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  endif()

  add_executable(${testexe}
    test_negotiate.c
    ${_element_srcs}
    mock_libuvc.c
  )
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
    PkgConfig::LIBUSB
    Threads::Threads
  )
  if(san_opts)
    target_compile_options(${testexe} PRIVATE ${san_opts})
    target_link_options(${testexe} PRIVATE ${san_opts})
  endif()

  # "<ctest name>:<gst-check test fn>" - one ctest entry each via GST_CHECKS. The
  # leak case is only meaningful (and only registered) for the asan variant.
  set(_neg_cases
    "negotiate_edge_zero_format${suffix}:test_negotiate_zero_format"
    "negotiate_edge_framerate${suffix}:test_negotiate_framerate_zero"
    "negotiate_edge_zero_interval${suffix}:test_negotiate_zero_interval"
  )
  if(NOT sanitizer STREQUAL "")
    list(APPEND _neg_cases "negotiate_leak:test_negotiate_leak")
  endif()

  foreach(_case ${_neg_cases})
    string(REPLACE ":" ";" _parts ${_case})
    list(GET _parts 0 _ctestname)
    list(GET _parts 1 _testfn)

    set(_neg_home "${CMAKE_BINARY_DIR}/negotiate-home-${_ctestname}")
    file(MAKE_DIRECTORY ${_neg_home})

    set(_neg_env
      "GST_PLUGIN_SYSTEM_PATH="
      "GST_REGISTRY=${CMAKE_BINARY_DIR}/negotiate-registry-${_ctestname}.bin"
      "GST_REGISTRY_FORK=no"
      "HOME=${_neg_home}"
      "CK_FORK=no"
      "GST_CHECKS=${_testfn}"
      "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
    )
    if(NOT sanitizer STREQUAL "")
      if(_ctestname STREQUAL "negotiate_leak")
        # detect_leaks must be ON for __lsan_do_recoverable_leak_check(), but the
        # at-exit check is disabled so only the in-test recoverable check (scoped
        # by __lsan_disable() and filtered by the suppressions file) is the gate.
        list(APPEND _neg_env
          "ASAN_OPTIONS=detect_leaks=1:leak_check_at_exit=0:abort_on_error=1:halt_on_error=1"
          "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_negotiate.suppressions:print_suppressions=0")
      else()
        list(APPEND _neg_env
          "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
      endif()
    endif()

    add_test(NAME ${_ctestname} COMMAND ${testexe})
    set_tests_properties(${_ctestname} PROPERTIES
      ENVIRONMENT "${_neg_env}"
      # start() binds the fixed /tmp/libuvc_control socket via the control thread,
      # so these must not run concurrently with each other or the other
      # start()-driven suites under `ctest -j`.
      RESOURCE_LOCK uvc_control_socket
      TIMEOUT 120
    )
  endforeach()
endfunction()

add_negotiate_variant("" "")
if(ENABLE_SANITIZERS)
  add_negotiate_variant("asan" "address")
endif()

# -----------------------------------------------------------------------------
# PTS clamp + restart-state tests (Task 14).
#
# Same single-executable, statically-registered shape as test_device_select so
# the element's frame_callback() PTS arithmetic and the start() restart-state
# reset run in the test process with the mock feeder driving frame_callback().
#
#   pts_monotonic  The mock feeds access units far faster than the negotiated
#                  frame interval, so the running PTS outruns the wall clock and
#                  the resync offset goes strongly negative; assert every buffer
#                  PTS is strictly increasing and every DURATION is sane (M4).
#   restart_idr    Lead each stream with non-IDR slices before the first IDR,
#                  stream once to latch had_idr, restart, and assert the first
#                  buffer after the restart is never a forwarded non-IDR (M5).
# -----------------------------------------------------------------------------
add_executable(test_pts_monotonic
  test_pts_monotonic.c
  ${_element_srcs}
  mock_libuvc.c
)
target_include_directories(test_pts_monotonic PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_pts_monotonic PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  PkgConfig::LIBUSB
  Threads::Threads
)
if(ENABLE_SANITIZERS)
  target_compile_options(test_pts_monotonic PRIVATE
    -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(test_pts_monotonic PRIVATE -fsanitize=address)
endif()

# "<ctest name>:<gst-check test fn>" - one ctest entry each, selected by GST_CHECKS.
set(_pts_monotonic_cases
  "pts_monotonic:test_pts_monotonic"
  "restart_idr:test_restart_idr"
)
foreach(_case ${_pts_monotonic_cases})
  string(REPLACE ":" ";" _parts ${_case})
  list(GET _parts 0 _ctestname)
  list(GET _parts 1 _testfn)

  set(_pm_home "${CMAKE_BINARY_DIR}/pts-monotonic-home-${_ctestname}")
  file(MAKE_DIRECTORY ${_pm_home})

  set(_pm_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/pts-monotonic-registry-${_ctestname}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_pm_home}"
    "CK_FORK=no"
    "GST_CHECKS=${_testfn}"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(ENABLE_SANITIZERS)
    list(APPEND _pm_env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  add_test(NAME ${_ctestname} COMMAND test_pts_monotonic)
  set_tests_properties(${_ctestname} PROPERTIES
    ENVIRONMENT "${_pm_env}"
    # start() binds the fixed /tmp/libuvc_control socket via the control thread,
    # so these must not run concurrently with each other or the other
    # start()-driven suites under `ctest -j`.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endforeach()

# -----------------------------------------------------------------------------
# V4L2 capability probe tests (Task 23).
#
# Compiles uvc_device.c directly into the test executable (no plugin .so, no
# mock) and calls gst_libuvc_h264_src_v4l2_probe() with indices guaranteed
# absent on any CI host. Verifies the probe is non-fatal and does not crash
# regardless of whether a real /dev/videoN node exists.
# -----------------------------------------------------------------------------
add_executable(test_v4l2_probe
  test_v4l2_probe.c
  ${CMAKE_SOURCE_DIR}/libuvch264src/src/uvc_device.c
)
target_include_directories(test_v4l2_probe PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_v4l2_probe PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::LIBUSB
  ${LIBUVC_LINK_TARGET}
)

set(_probe_env
  "GST_PLUGIN_SYSTEM_PATH="
  "GST_REGISTRY=${CMAKE_BINARY_DIR}/v4l2probe-registry.bin"
  "GST_REGISTRY_FORK=no"
  "HOME=${CMAKE_BINARY_DIR}"
)
if(LIBUVC_RUNTIME_DIR)
  list(APPEND _probe_env "LD_LIBRARY_PATH=${LIBUVC_RUNTIME_DIR}")
endif()

add_test(NAME v4l2_probe COMMAND test_v4l2_probe probe)
set_tests_properties(v4l2_probe PROPERTIES
  ENVIRONMENT "${_probe_env}"
  TIMEOUT 30
)

add_test(NAME v4l2_probe_nonfatal COMMAND test_v4l2_probe nonfatal)
set_tests_properties(v4l2_probe_nonfatal PROPERTIES
  ENVIRONMENT "${_probe_env}"
  TIMEOUT 30
)

# -----------------------------------------------------------------------------
# Live-source polish tests (Task 19).
#
# Same single-executable, statically-registered shape as test_pts_monotonic so
# the test can drive a real PLAYING pipeline against the mock feeder AND reach
# into the instance struct / call frame_callback() directly.
#   live_source_polish      LATENCY query reports live + min == one frame
#                           interval, and buffer OFFSETs are a strict +1 sequence.
#   spspps_write_on_change  The SPS/PPS cache is written on a changed parameter
#                           set, not rewritten for an identical one (L10).
# -----------------------------------------------------------------------------
add_executable(test_live_source
  test_live_source.c
  ${_element_srcs}
  mock_libuvc.c
)
target_include_directories(test_live_source PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_live_source PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  PkgConfig::LIBUSB
  Threads::Threads
)
if(ENABLE_SANITIZERS)
  target_compile_options(test_live_source PRIVATE
    -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(test_live_source PRIVATE -fsanitize=address)
endif()

set(_live_source_cases
  "live_source_polish:test_live_source_polish"
  "spspps_write_on_change:test_spspps_write_on_change"
)
foreach(_case ${_live_source_cases})
  string(REPLACE ":" ";" _parts ${_case})
  list(GET _parts 0 _ctestname)
  list(GET _parts 1 _testfn)

  set(_ls_home "${CMAKE_BINARY_DIR}/live-source-home-${_ctestname}")
  file(MAKE_DIRECTORY ${_ls_home})

  set(_ls_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/live-source-registry-${_ctestname}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_ls_home}"
    "CK_FORK=no"
    "GST_CHECKS=${_testfn}"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(ENABLE_SANITIZERS)
    list(APPEND _ls_env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  add_test(NAME ${_ctestname} COMMAND test_live_source)
  set_tests_properties(${_ctestname} PROPERTIES
    ENVIRONMENT "${_ls_env}"
    # start() may spawn the control thread, so keep these off the shared socket
    # resource like the other start()-driven suites under `ctest -j`.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endforeach()

# -----------------------------------------------------------------------------
# NAL parser unit tests (Task 15). Pure C against the real parser in
# frame_pipeline.c: no UVC device, no GStreamer runtime, no mock feeder. The TU
# is linked whole, so frame_callback()'s only element-internal references (the
# debug category and store_spspps) are satisfied by stubs in test_nal_parse.c;
# the parser functions under test touch neither. Three argv-selected suites; the
# ASAN variant is the teeth for the bounds suite (over-read on truncated /
# oversized / merged NAL buffers).
# -----------------------------------------------------------------------------
function(add_nal_parse_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(testexe "test_nal_parse${suffix}")

  set(san_opts "")
  if(NOT sanitizer STREQUAL "")
    set(san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  endif()

  add_executable(${testexe}
    test_nal_parse.c
    ${CMAKE_SOURCE_DIR}/libuvch264src/src/frame_pipeline.c
  )
  target_include_directories(${testexe} PRIVATE
    ${CMAKE_SOURCE_DIR}/libuvch264src/src
    ${LIBUVC_EXTRA_INCLUDES}
    ${LIBUVC_INCLUDE_DIRS}
  )
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
  )
  if(san_opts)
    target_compile_options(${testexe} PRIVATE ${san_opts})
    target_link_options(${testexe} PRIVATE ${san_opts})
  endif()

  set(_nal_env "")
  if(sanitizer STREQUAL "address")
    set(_nal_env "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  foreach(_suite multislice startcode bounds)
    add_test(NAME nal_parse_${_suite}${suffix} COMMAND ${testexe} ${_suite})
    if(_nal_env)
      set_tests_properties(nal_parse_${_suite}${suffix} PROPERTIES ENVIRONMENT "${_nal_env}")
    endif()
  endforeach()
endfunction()

add_nal_parse_variant("" "")
if(ENABLE_SANITIZERS)
  add_nal_parse_variant("asan" "address")
endif()

# -----------------------------------------------------------------------------
# Consolidated functional suite (Task 20).
#
# Reuses the mock-backed plugin built by add_mock_harness_variant (loaded via an
# isolated GST_PLUGIN_PATH, exactly like the smoke test) and fills the gaps the
# per-task suites leave: the POSITIVE caps-negotiation matrix (H.264 -> video/
# x-h264, H.265 -> video/x-h265, frames flow in both) and frame delivery under
# downstream backpressure. The H.265 case selects the codec purely via the
# MOCK_UVC_FRAME_FORMAT env var the mock reads at uvc_init() - the same .so backs
# every case. Built in plain, ASAN, and TSAN variants so the full functional path
# is exercised under both sanitizers in CI.
# -----------------------------------------------------------------------------
function(add_functional_variant variant sanitizer)
  if(variant STREQUAL "")
    set(suffix "")
  else()
    set(suffix "_${variant}")
  endif()

  set(plugin "gstlibuvch264src_mock${suffix}")   # built by add_mock_harness_variant
  set(testexe "test_functional${suffix}")
  set(plugdir "${CMAKE_BINARY_DIR}/gstreamer-1.0-mock${suffix}")

  set(san_opts "")
  if(NOT sanitizer STREQUAL "")
    set(san_opts -fsanitize=${sanitizer} -fno-omit-frame-pointer -g)
  endif()

  add_executable(${testexe} test_functional.c)
  target_link_libraries(${testexe} PRIVATE
    PkgConfig::GST
    PkgConfig::GST_BASE
    PkgConfig::GST_CHECK
  )
  add_dependencies(${testexe} ${plugin})
  if(san_opts)
    target_compile_options(${testexe} PRIVATE ${san_opts})
    target_link_options(${testexe} PRIVATE ${san_opts})
  endif()

  set(_home "${CMAKE_BINARY_DIR}/functional-home${suffix}")
  file(MAKE_DIRECTORY ${_home})
  set(_env
    "GST_PLUGIN_PATH=${plugdir}"
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/functional-registry${suffix}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_home}"
    "CK_FORK=no"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(sanitizer STREQUAL "address")
    list(APPEND _env "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  elseif(sanitizer STREQUAL "thread")
    list(APPEND _env
      "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.suppressions:halt_on_error=1:ignore_noninstrumented_modules=1")
  endif()

  # "<ctest name>:<gst-check test fn>:<extra env>" - one ctest entry each via
  # GST_CHECKS. The H.265 case adds MOCK_UVC_FRAME_FORMAT=H265 so the same mock
  # plugin advertises and feeds H.265.
  set(_functional_cases
    "functional_caps_h264${suffix}:test_caps_h264:"
    "functional_caps_h265${suffix}:test_caps_h265:MOCK_UVC_FRAME_FORMAT=H265"
    "functional_backpressure${suffix}:test_backpressure:"
  )
  foreach(_case ${_functional_cases})
    string(REPLACE ":" ";" _parts ${_case})
    list(GET _parts 0 _ctestname)
    list(GET _parts 1 _testfn)
    list(GET _parts 2 _extraenv)

    set(_case_env "${_env};GST_CHECKS=${_testfn}")
    if(NOT _extraenv STREQUAL "")
      set(_case_env "${_case_env};${_extraenv}")
    endif()

    add_test(NAME ${_ctestname} COMMAND ${testexe})
    set_tests_properties(${_ctestname} PROPERTIES
      ENVIRONMENT "${_case_env}"
      # Shares the mock plugin with the other mock variants; keep it off the
      # shared control-socket resource like the rest under `ctest -j`.
      RESOURCE_LOCK uvc_control_socket
      TIMEOUT 120
    )
  endforeach()
endfunction()

add_functional_variant("" "")
if(ENABLE_SANITIZERS)
  add_functional_variant("asan" "address")
  add_functional_variant("tsan" "thread")
endif()

# -----------------------------------------------------------------------------
# Disconnect-detection + opt-in reconnect tests (Task 18).
#
# Same single-executable, statically-registered shape as test_pts_monotonic so
# the mock feeder drives the real create() disconnect path in-process and the
# mock's uvc_open() counter is observable (mock_uvc_open_count). Each case is its
# own ctest entry via GST_CHECKS so it gets its own mock configuration.
#   disconnect_error  reconnect off (default): a silent source posts a
#                     RESOURCE/READ bus error within the timeout window.
#   reconnect_resume  reconnect on: the element reopens the device and frames
#                     resume after a simulated replug instead of erroring.
# -----------------------------------------------------------------------------
add_executable(test_reconnect
  test_reconnect.c
  ${_element_srcs}
  mock_libuvc.c
)
target_include_directories(test_reconnect PRIVATE
  ${CMAKE_SOURCE_DIR}/libuvch264src/src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LIBUVC_EXTRA_INCLUDES}
  ${LIBUVC_INCLUDE_DIRS}
)
target_link_libraries(test_reconnect PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::GST_CHECK
  PkgConfig::LIBUSB
  Threads::Threads
)
if(ENABLE_SANITIZERS)
  target_compile_options(test_reconnect PRIVATE
    -fsanitize=address -fno-omit-frame-pointer -g)
  target_link_options(test_reconnect PRIVATE -fsanitize=address)
endif()

set(_reconnect_cases
  "disconnect_error:test_disconnect_error"
  "reconnect_resume:test_reconnect_resume"
)
foreach(_case ${_reconnect_cases})
  string(REPLACE ":" ";" _parts ${_case})
  list(GET _parts 0 _ctestname)
  list(GET _parts 1 _testfn)

  set(_rc_home "${CMAKE_BINARY_DIR}/reconnect-home-${_ctestname}")
  file(MAKE_DIRECTORY ${_rc_home})

  set(_rc_env
    "GST_PLUGIN_SYSTEM_PATH="
    "GST_REGISTRY=${CMAKE_BINARY_DIR}/reconnect-registry-${_ctestname}.bin"
    "GST_REGISTRY_FORK=no"
    "HOME=${_rc_home}"
    "CK_FORK=no"
    "GST_CHECKS=${_testfn}"
    "GST_COREELEMENTS_PLUGIN=${GST_PLUGINS_DIR}/libgstcoreelements.so"
  )
  if(ENABLE_SANITIZERS)
    list(APPEND _rc_env
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1")
  endif()

  add_test(NAME ${_ctestname} COMMAND test_reconnect)
  set_tests_properties(${_ctestname} PROPERTIES
    ENVIRONMENT "${_rc_env}"
    # start() may spawn the control thread, so keep these off the shared socket
    # resource like the other start()-driven suites under `ctest -j`.
    RESOURCE_LOCK uvc_control_socket
    TIMEOUT 120
  )
endforeach()
