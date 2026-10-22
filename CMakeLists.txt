cmake_minimum_required(VERSION 3.21)
project(WirePlay VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# FFmpeg is LGPL-2.1+ and is linked dynamically (the vcpkg port builds shared
# libraries by default), which is what keeps this project's MIT licence intact.
# Do not switch it to a static build without reading docs/LEGAL.md.
# SDL2 is zlib-licensed. Neither carries a royalty obligation: we only *decode*
# HEVC and AAC, and the device does the encoding.
# ---------------------------------------------------------------------------
find_package(FFmpeg QUIET)
if(NOT FFmpeg_FOUND)
  # vcpkg exposes each FFmpeg component as its own package.
  find_package(unofficial-ffmpeg CONFIG QUIET)
endif()

find_package(PkgConfig QUIET)

set(FFMPEG_LIBS "")
set(FFMPEG_INCLUDES "")

if(TARGET FFmpeg::avcodec OR TARGET unofficial::ffmpeg::avcodec)
  if(TARGET FFmpeg::avcodec)
    set(FFMPEG_LIBS FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample)
  else()
    set(FFMPEG_LIBS unofficial::ffmpeg::avcodec unofficial::ffmpeg::avutil
                    unofficial::ffmpeg::swscale unofficial::ffmpeg::swresample)
  endif()
elseif(PkgConfig_FOUND)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
                    libavcodec libavutil libswscale libswresample)
  set(FFMPEG_LIBS PkgConfig::FFMPEG)
else()
  find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
  find_library(AVCODEC_LIBRARY avcodec REQUIRED)
  find_library(AVUTIL_LIBRARY avutil REQUIRED)
  find_library(SWSCALE_LIBRARY swscale REQUIRED)
  find_library(SWRESAMPLE_LIBRARY swresample REQUIRED)
  set(FFMPEG_INCLUDES ${FFMPEG_INCLUDE_DIR})
  set(FFMPEG_LIBS ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY})
endif()

find_package(SDL2 QUIET)
set(SDL2_LIBS "")
if(TARGET SDL2::SDL2)
  set(SDL2_LIBS SDL2::SDL2)
  if(TARGET SDL2::SDL2main)
    list(APPEND SDL2_LIBS SDL2::SDL2main)
  endif()
elseif(PkgConfig_FOUND)
  pkg_check_modules(SDL2PC REQUIRED IMPORTED_TARGET sdl2)
  set(SDL2_LIBS PkgConfig::SDL2PC)
else()
  message(FATAL_ERROR "SDL2 not found. On Windows: vcpkg install sdl2")
endif()

# ---------------------------------------------------------------------------
# The application (MIT)
# ---------------------------------------------------------------------------
add_executable(wireplay
  src/main.cpp
  src/wp/protocol.cpp
  src/wp/decoder.cpp
  src/wp/framebus.cpp
  src/wp/process.cpp
)
target_include_directories(wireplay PRIVATE src ${FFMPEG_INCLUDES})
target_link_libraries(wireplay PRIVATE ${SDL2_LIBS} ${FFMPEG_LIBS})

# Optional Vulkan preview (--renderer vulkan). Built only when a Vulkan
# SDK/loader is visible; the D3D11/SDL preview remains the default and
# the rollback when this is absent.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
  # APPEND PROPERTY rather than a second target_compile_definitions call:
  # engine/tests/test_windows_branches.py reads the first such call per
  # target to verify the MSVC NOMINMAX block, and must keep finding it.
  set_property(TARGET wireplay APPEND PROPERTY COMPILE_DEFINITIONS WIREPLAY_HAVE_VULKAN)
  target_link_libraries(wireplay PRIVATE Vulkan::Vulkan)

# OpenGL preview renderer (--renderer opengl): fixed-function GL 1.1,
# so plain opengl32 on Windows / libGL elsewhere is all it needs.
if(WIN32)
    target_link_libraries(wireplay PRIVATE opengl32)
else()
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
        target_link_libraries(wireplay PRIVATE OpenGL::GL)
    endif()
endif()
endif()

if(WIN32)
  # Console subsystem, deliberately. A GUI-subsystem executable is not waited
  # on by PowerShell: the prompt comes back immediately, the app's output
  # lands after it, and the user has to press Enter to get a usable prompt
  # back when the run ends -- which read as "it hangs after the done line" on
  # every single run. The mirror is driven from a terminal, so behaving like
  # a terminal program is worth more than hiding a console on double-click.
  target_link_libraries(wireplay PRIVATE ws2_32 d3d11 d3d12 dxgi d3dcompiler)
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(wireplay PRIVATE pthread rt)
endif()

# ---------------------------------------------------------------------------
# The virtual camera (DirectShow source filter, Windows only)
#
# Needs the Windows SDK baseclasses, which ship as source rather than as a
# library: find them, build streams.lib once, then link. OBS, Streamlabs,
# Discord and Chrome all enumerate DirectShow video-capture filters, which is
# why this is DirectShow and not the Media Foundation virtual camera.
# ---------------------------------------------------------------------------
# The virtual camera: a DirectShow source filter, Windows only.
#
# It is written against raw COM rather than the DirectShow baseclasses, because
# the baseclasses ship with the SDK *samples* and current SDKs do not include
# them -- a filter that needs an optional SDK component is a filter that does
# not build. Everything it needs is in dshow.h plus strmiids.
option(WIREPLAY_BUILD_CAMERA "Build the DirectShow virtual camera filter" ${WIN32})
if(WIREPLAY_BUILD_CAMERA AND WIN32)
  add_library(wireplay_camera SHARED src/dshow_filter.cpp src/wp/framebus.cpp
              src/WirePlayCamera.def)
  target_include_directories(wireplay_camera PRIVATE src)
  target_compile_definitions(wireplay_camera PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(wireplay_camera PRIVATE strmiids ole32 oleaut32 uuid winmm)
  set_target_properties(wireplay_camera PROPERTIES OUTPUT_NAME "WirePlayCamera")

  # Registering writes to HKLM, so it needs an elevated shell. Deliberately NOT
  # a post-build step: a build from a normal prompt would otherwise fail at the
  # very end and look like a compile error.
  add_executable(wireplay_dshow_probe tools/dshow_probe.cpp)
  target_compile_definitions(wireplay_dshow_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(wireplay_dshow_probe PRIVATE strmiids ole32 oleaut32 uuid)

  add_custom_target(register_camera
    COMMAND regsvr32 /s "$<TARGET_FILE:wireplay_camera>"
    DEPENDS wireplay_camera
    COMMENT "Registering the WirePlay virtual camera (run from an elevated shell)"
    VERBATIM)
endif()

if(MSVC)
  # NOMINMAX and WIN32_LEAN_AND_MEAN must be set on the command line, not only
  # inside framebus.hpp: whichever header pulls in windows.h first wins, and a
  # translation unit that includes an SDL or FFmpeg header before ours would
  # still get the min/max macros and fail with C2589 on every std::min.
  target_compile_definitions(wireplay PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(wireplay PRIVATE /W4 /permissive-)
else()
  target_compile_options(wireplay PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(WIREPLAY_BUILD_TESTS "Build the C++ unit tests" ON)
if(WIREPLAY_BUILD_TESTS)
  enable_testing()
  add_executable(wireplay_tests
    tests/cpp/test_main.cpp
    tests/cpp/test_protocol.cpp
    src/wp/protocol.cpp
    )
  target_include_directories(wireplay_tests PRIVATE src)
  target_compile_definitions(wireplay_tests PRIVATE
    WIREPLAY_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/engine/tests/data")
  if(NOT MSVC)
    target_compile_options(wireplay_tests PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME wireplay_tests COMMAND wireplay_tests)

  # The virtual camera's shared-memory bus and its media-type logic are
  # deliberately free of Windows headers so they can be tested here, in CI, on
  # Linux. The COM shell around them cannot be, and is not.
  add_executable(wireplay_framebus_tests
    tests/cpp/test_framebus.cpp
    src/wp/framebus.cpp)
  target_include_directories(wireplay_framebus_tests PRIVATE src)
  if(UNIX AND NOT APPLE)
    target_link_libraries(wireplay_framebus_tests PRIVATE pthread rt)
  endif()
  if(MSVC)
    target_compile_definitions(wireplay_framebus_tests PRIVATE
      NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(wireplay_framebus_tests PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME wireplay_framebus_tests COMMAND wireplay_framebus_tests)

  # Stands in for the DirectShow filter: a second process that pulls from the
  # bus at 60 Hz. Lets the camera path be exercised end to end without Windows,
  # which is the only way to test it here at all.
  add_executable(wireplay_camera_probe
    tools/camera_probe.cpp
    src/wp/framebus.cpp)
  target_include_directories(wireplay_camera_probe PRIVATE src)
  if(UNIX AND NOT APPLE)
    target_link_libraries(wireplay_camera_probe PRIVATE pthread rt)
  endif()
  if(MSVC)
    target_compile_definitions(wireplay_camera_probe PRIVATE
      NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  if(NOT MSVC)
    target_compile_options(wireplay_camera_probe PRIVATE -Wall -Wextra)
  endif()

  add_executable(wireplay_backend_tests tests/cpp/test_backend.cpp)
  target_include_directories(wireplay_backend_tests PRIVATE src)
  if(MSVC)
    target_compile_definitions(wireplay_backend_tests PRIVATE
      NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(wireplay_backend_tests PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME wireplay_backend_tests COMMAND wireplay_backend_tests)

  # The filter itself is Windows-only, but compiling it against the stub Win32
  # headers catches typos, missing overrides and wrong signatures on every CI
  # run. This is a type check, not a conformance test: the stubs are a
  # transcription of the COM interfaces, so a misremembered API would be wrong
  # in both places and pass.
  # NOT WIN32, and that condition is load-bearing: the stub directory contains a
  # header named windows.h, so adding it to the include path on Windows would
  # shadow the real one and strip the build of every Win32 API. On Windows the
  # filter is compiled for real by the wireplay_camera target instead.
  if(NOT WIN32)
    add_library(wireplay_camera_syntax OBJECT src/dshow_filter.cpp src/wp/framebus.cpp)
    target_include_directories(wireplay_camera_syntax PRIVATE src tests/win32_stub)
    target_compile_options(wireplay_camera_syntax PRIVATE -Wall -Wextra)
  endif()

  add_executable(wireplay_camera_tests tests/cpp/test_camera_types.cpp)
  target_include_directories(wireplay_camera_tests PRIVATE src)
  if(MSVC)
    target_compile_definitions(wireplay_camera_tests PRIVATE
      NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(wireplay_camera_tests PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME wireplay_camera_tests COMMAND wireplay_camera_tests)
endif()

install(TARGETS wireplay RUNTIME DESTINATION bin)
if(TARGET wireplay_camera)
  install(TARGETS wireplay_camera RUNTIME DESTINATION bin)
endif()
