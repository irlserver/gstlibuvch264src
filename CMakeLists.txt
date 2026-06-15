# =============================================================================
# CMake build for the libuvch264src plugin-load smoke test (ctest target).
#
# SCOPE: This CMake project exists ONLY to compile the GStreamer plugin together
# with a hardware-independent gst-check smoke test and expose it via ctest.
#
# The CANONICAL production build remains Meson (libuvch264src/meson.build) and
# the Dockerfile. Device-image artifacts must NOT be produced from this build.
# The plugin source is compiled verbatim - no runtime behavior is changed here.
# =============================================================================

cmake_minimum_required(VERSION 3.16)
project(gstlibuvch264src C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

find_package(PkgConfig REQUIRED)
# Version floors. GStreamer 1.14 is the oldest series the plugin's pad/caps API
# usage compiles against (Ubuntu 18.04 baseline); no known incompatibility on
# the 1.x line, so no upper bound is imposed. libusb 1.0.20 is the floor for the
# auto-detach-kernel-driver call the libuvc patches rely on.
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.14)
pkg_check_modules(GST_BASE REQUIRED IMPORTED_TARGET gstreamer-base-1.0>=1.14)
pkg_check_modules(GST_CHECK REQUIRED IMPORTED_TARGET gstreamer-check-1.0>=1.14)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0>=1.0.20)

# -----------------------------------------------------------------------------
# libuvc dependency.
#
# Prefer a system/pkg-config libuvc; otherwise vendor it at build time exactly
# like the Dockerfile does (upstream v0.0.7 + the UVC 1.5 and H.265 patches the
# plugin relies on). LIBUVC_RUNTIME_DIR is exported to the test so the vendored
# shared object is resolvable at dlopen() time.
# -----------------------------------------------------------------------------
pkg_check_modules(LIBUVC IMPORTED_TARGET libuvc)

if(LIBUVC_FOUND)
  set(LIBUVC_LINK_TARGET PkgConfig::LIBUVC)
  set(LIBUVC_EXTRA_INCLUDES "")
  set(LIBUVC_RUNTIME_DIR "")
else()
  message(STATUS "libuvc not found via pkg-config; vendoring upstream v0.0.7 + patches")
  include(FetchContent)

  # libuvc v0.0.7 ships pre-3.5 CMake policies; allow them under modern CMake.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
  set(BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(BUILD_TEST OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)

  FetchContent_Declare(libuvc
    GIT_REPOSITORY https://github.com/libuvc/libuvc.git
    # Pinned to the v0.0.7 commit SHA (tags are mutable). Keep in sync with the Dockerfile.
    GIT_TAG 68d07a00e11d1944e27b7295ee69673239c00b4b
    PATCH_COMMAND patch -p1 < ${CMAKE_CURRENT_SOURCE_DIR}/patches/uvc15-support.patch
      && patch -p1 < ${CMAKE_CURRENT_SOURCE_DIR}/patches/libuvc-h265-support.patch
    UPDATE_DISCONNECTED 1
  )
  FetchContent_MakeAvailable(libuvc)

  set(LIBUVC_LINK_TARGET uvc)
  # v0.0.7 uses directory-level include_directories, so expose them explicitly.
  set(LIBUVC_EXTRA_INCLUDES
    ${libuvc_SOURCE_DIR}/include
    ${libuvc_BINARY_DIR}/include
  )
  set(LIBUVC_RUNTIME_DIR ${libuvc_BINARY_DIR})
endif()

# -----------------------------------------------------------------------------
# GStreamer plugin module (compiled from the unmodified plugin source).
# Output: <build>/gstreamer-1.0/libgstlibuvch264src.so
# -----------------------------------------------------------------------------
set(GST_PLUGIN_BUILD_DIR ${CMAKE_BINARY_DIR}/gstreamer-1.0)

add_library(gstlibuvch264src MODULE
  libuvch264src/src/gstlibuvch264src.c
  libuvch264src/src/uvc_device.c
  libuvch264src/src/frame_pipeline.c
  libuvch264src/src/ptz_control.c
  libuvch264src/src/spspps_cache.c
  libuvch264src/src/gstlibuvch264src_error.c
)
set_target_properties(gstlibuvch264src PROPERTIES
  PREFIX "lib"
  LIBRARY_OUTPUT_DIRECTORY ${GST_PLUGIN_BUILD_DIR}
)
target_include_directories(gstlibuvch264src PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/libuvch264src/src
  ${LIBUVC_EXTRA_INCLUDES}
)
target_link_libraries(gstlibuvch264src PRIVATE
  PkgConfig::GST
  PkgConfig::GST_BASE
  PkgConfig::LIBUSB
  ${LIBUVC_LINK_TARGET}
)
if(LIBUVC_RUNTIME_DIR)
  # Make the vendored libuvc resolvable for the test even outside ctest's env.
  set_target_properties(gstlibuvch264src PROPERTIES
    BUILD_RPATH ${LIBUVC_RUNTIME_DIR}
  )
endif()

add_subdirectory(tests)
