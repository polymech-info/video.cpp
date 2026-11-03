cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pm-video
    VERSION 0.1.0
    DESCRIPTION "PolyMech portable video"
    LANGUAGES CXX)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
  include(FetchContent)
  set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

set(PM_VIDEO_TANIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
  "Repo root that holds src/lib/onnx, packages/nvidia, and packages/mediapipe-tasks")
set(PM_VIDEO_DIST_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE PATH
  "Directory for tanit_vcam_source and pm-video-nvenc")
option(PM_VIDEO_RECORD_ENGINES "Screen capture, file encoder, and session recorder" OFF)
option(PM_VIDEO_HOST_SINGLETONS "Host compiles hooks.cpp and captions.cpp once" OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/cmake/host-layout.cmake")
  set(_PM_VIDEO_LINK_COMMONS_DEFAULT ON)
else()
  set(_PM_VIDEO_LINK_COMMONS_DEFAULT OFF)
endif()
option(PM_VIDEO_LINK_COMMONS "Link sibling packages/commons.cpp." ${_PM_VIDEO_LINK_COMMONS_DEFAULT})

set(PM_VIDEO_SOURCES
  src/dynlib.cpp
  src/devices.cpp
  src/mjpeg_avi.cpp
  src/filters/image_ops.cpp
  src/vcam/convert.cpp
  src/streaming/session.cpp
  src/detect/commons.cpp)

if(NOT PM_VIDEO_HOST_SINGLETONS)
  list(APPEND PM_VIDEO_SOURCES src/hooks.cpp)
endif()

if(WIN32)
  list(APPEND PM_VIDEO_SOURCES
    src/capture_win.cpp
    src/vcam/sink.cpp
    src/preview.cpp
    src/idle.cpp
    src/zoom.cpp
    src/overlay.cpp
    src/filters/face_effects.cpp
    src/filters/bg_effects.cpp
    src/filters/beauty_processor.cpp
    src/filters/face_landmarker.cpp
    src/filters/image_segmenter.cpp
    src/filters/skin_gpu.cpp
    src/nvenc/loader.cpp)
  if(PM_VIDEO_RECORD_ENGINES)
    list(APPEND PM_VIDEO_SOURCES
      src/screen.cpp
      src/frame_pump_win.cpp
      src/encoder.cpp
      src/mf_audio_mux.cpp
      src/record_session.cpp
      src/session_recorder.cpp
      src/window_bounds.cpp
      src/display_affinity.cpp)
  endif()
endif()
if(UNIX AND NOT APPLE)
  list(APPEND PM_VIDEO_SOURCES src/capture_linux.cpp)
endif()
if(APPLE)
  list(APPEND PM_VIDEO_SOURCES src/capture_osx.mm)
endif()

set(_pm_video_onnx "${PM_VIDEO_TANIT_ROOT}/src/lib/onnx/OnnxSession.hpp")
if(EXISTS "${_pm_video_onnx}")
  list(APPEND PM_VIDEO_SOURCES
    src/detect/commons_onnx.cpp
    src/detect/boxes.cpp
    src/detect/cls.cpp
    src/detect/obb.cpp
    src/detect/pose.cpp
    src/detect/seg.cpp
    src/detect/render.cpp
    src/detect/serialize.cpp)
endif()

if(TARGET pm-audio-lib AND NOT PM_VIDEO_HOST_SINGLETONS)
  list(APPEND PM_VIDEO_SOURCES src/captions.cpp)
endif()

add_library(pm-video-lib STATIC ${PM_VIDEO_SOURCES})
target_include_directories(pm-video-lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(pm-video-lib PUBLIC cxx_std_20)
target_compile_definitions(pm-video-lib PRIVATE FEATURE_VIDEO=1)
target_link_libraries(pm-video-lib PUBLIC nlohmann_json::nlohmann_json)
if(PM_VIDEO_LINK_COMMONS)
  set(_pm_commons_layout "${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/cmake/host-layout.cmake")
  if(NOT EXISTS "${_pm_commons_layout}")
    message(FATAL_ERROR "PM_VIDEO_LINK_COMMONS is ON but ${_pm_commons_layout} is missing")
  endif()
  include("${_pm_commons_layout}")
  pm_commons_add_sibling("${CMAKE_CURRENT_BINARY_DIR}/pm-commons-lib")
  target_link_libraries(pm-video-lib PUBLIC pm-commons-lib)
endif()

if(EXISTS "${_pm_video_onnx}")
  target_include_directories(pm-video-lib PRIVATE "${PM_VIDEO_TANIT_ROOT}/src")
endif()
if(EXISTS "${PM_VIDEO_TANIT_ROOT}/packages/mediapipe-tasks/bridge/pixlwiz_face_landmarker.h")
  target_include_directories(pm-video-lib PRIVATE
    "${PM_VIDEO_TANIT_ROOT}/packages/mediapipe-tasks/bridge")
endif()

if(WIN32)
  target_link_libraries(pm-video-lib PUBLIC
    mf mfplat mfreadwrite mfuuid ole32 windowscodecs
    d3d11 dxgi d3dcompiler gdiplus dwmapi strmiids advapi32 shell32)
endif()
if(UNIX AND NOT APPLE)
  find_package(JPEG REQUIRED)
  target_link_libraries(pm-video-lib PUBLIC JPEG::JPEG)
endif()
if(APPLE)
  target_link_libraries(pm-video-lib PUBLIC
    "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo"
    "-framework ImageIO" "-framework Foundation" "-framework CoreGraphics")
endif()
if(MSVC)
  target_compile_definitions(pm-video-lib PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  target_compile_options(pm-video-lib PRIVATE /W3 /utf-8 /EHsc)
else()
  target_compile_options(pm-video-lib PRIVATE -Wall -Wextra)
endif()

if(WIN32 AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_library(tanit_vcam_source SHARED
    src/vcam/source.cpp
    src/vcam/source.def)
  target_compile_features(tanit_vcam_source PRIVATE cxx_std_20)
  target_compile_definitions(tanit_vcam_source PRIVATE
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_include_directories(tanit_vcam_source PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(tanit_vcam_source PRIVATE mfplat mfuuid ole32 mfsensorgroup)
  set_target_properties(tanit_vcam_source PROPERTIES
    OUTPUT_NAME "tanit_vcam_source" PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${PM_VIDEO_DIST_DIR}")

  set(_NV_ROOT "${PM_VIDEO_TANIT_ROOT}/packages/nvidia")
  set(_NV_ENC "${_NV_ROOT}/Samples/NvCodec/NvEncoder")
  if(EXISTS "${_NV_ROOT}/Interface/nvEncodeAPI.h" AND EXISTS "${_NV_ENC}/NvEncoder.cpp")
    add_library(pm-video-nvenc SHARED
      src/nvenc/plugin.cpp
      src/nvenc/driver.cpp
      "${_NV_ENC}/NvEncoder.cpp"
      "${_NV_ENC}/NvEncoderD3D11.cpp")
    target_compile_features(pm-video-nvenc PRIVATE cxx_std_17)
    target_compile_definitions(pm-video-nvenc PRIVATE PM_NVENC_PLUGIN_BUILD=1 _CRT_SECURE_NO_WARNINGS)
    target_include_directories(pm-video-nvenc PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${_NV_ROOT}/Interface"
      "${_NV_ROOT}/Samples/Utils"
      "${_NV_ROOT}/Samples/NvCodec")
    target_link_libraries(pm-video-nvenc PRIVATE d3d11 dxgi)
    set_target_properties(pm-video-nvenc PROPERTIES
      OUTPUT_NAME "pm-video-nvenc" PREFIX ""
      RUNTIME_OUTPUT_DIRECTORY "${PM_VIDEO_DIST_DIR}")
  endif()
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(NOT TARGET CLI11::CLI11)
    include(FetchContent)
    set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(cli11
      GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
      GIT_TAG v2.4.2
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(cli11)
  endif()

  add_executable(pm-video cli/main.cpp)
  target_link_libraries(pm-video PRIVATE pm-video-lib CLI11::CLI11)
  target_compile_features(pm-video PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_definitions(pm-video PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(pm-video PRIVATE /W3 /utf-8 /EHsc)
  else()
    target_compile_options(pm-video PRIVATE -Wall -Wextra)
  endif()
endif()
