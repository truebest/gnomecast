cmake_minimum_required(VERSION 3.19)
project(gnomecast_native C)
include(CTest)
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(HELLOLG_LINK_RDP_FFI "Link the Rust RDP FFI static library" OFF)
option(HELLOLG_WITH_SS4S "Build with ss4s decoder integration" OFF)
option(HELLOLG_WITH_SDL "Build the native SDL lifecycle/input loop" OFF)
set(HELLOLG_WITH_PRECONNECT_UI_DEFAULT OFF)
if(HELLOLG_WITH_SDL)
  set(HELLOLG_WITH_PRECONNECT_UI_DEFAULT ON)
endif()
option(HELLOLG_WITH_PRECONNECT_UI "Build the LVGL pre-connect UI for SDL product builds" ${HELLOLG_WITH_PRECONNECT_UI_DEFAULT})
option(HELLOLG_SS4S_WEBOS_MODULES_ONLY "Build only ss4s webOS hardware-video modules for MVP" ON)

set(HELLOLG_SS4S_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ss4s" CACHE PATH "Path to the pinned ss4s checkout")
set(HELLOLG_COMMONS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/commons" CACHE PATH "Path to the pinned commons-c checkout")
set(HELLOLG_LVGL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lvgl" CACHE PATH "Path to the pinned LVGL checkout")

get_filename_component(HELLOLG_C_COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)
set(HELLOLG_TARGET_WEBOS OFF)
if(HELLOLG_C_COMPILER_NAME MATCHES "arm-webos-linux-gnueabi-.*" OR CMAKE_C_COMPILER_TARGET MATCHES "(i[3-6]86)-webos-linux-gnu")
  set(HELLOLG_TARGET_WEBOS ON)
endif()

# Default ON only for product (webOS) builds: ExternalOPUS downloads the libopus source
# tarball at build time, which would break the documented offline host smoke build.
# Host test runs opt in with -DHELLOLG_WITH_OPUS=ON to cover the audio-opus ctest.
option(HELLOLG_WITH_OPUS "Decode rdpsnd Opus streams in-process for the audio mixer" ${HELLOLG_TARGET_WEBOS})

if(HELLOLG_TARGET_WEBOS)
  if(NOT HELLOLG_LINK_RDP_FFI)
    message(FATAL_ERROR "webOS product builds require HELLOLG_LINK_RDP_FFI=ON")
  endif()
  # System-volume bridge (mixer MASTER fader) talks to the Luna bus via the NATIVE
  # luna-service2 client. A luna-send-pub subprocess is NOT an option: fork()ing this
  # process while the NDL/OMX pipeline is (re)loading raced its in-process decoder
  # state (black screens observed live), and a pipe cannot deliver subscribe events.
  set(HELLOLG_WITH_LS2 ON)
  if(NOT HELLOLG_WITH_SS4S)
    message(FATAL_ERROR "webOS product builds require HELLOLG_WITH_SS4S=ON for H.264 hardware decode")
  endif()
  if(NOT HELLOLG_WITH_SDL)
    message(FATAL_ERROR "webOS product builds require HELLOLG_WITH_SDL=ON for native input/RemoteFX presentation")
  endif()
endif()

add_executable(gnomecast-native
  src/main.c
  src/au_snapshot.c
  src/luna_volume.c
  src/config_paths.c
  src/settings_json.c
  src/media_ss4s.c
  src/video_rgba_sdl.c
  src/video_ss4s.c
  src/audio_ss4s.c
  src/audio_mixer.c
  src/audio_opus.c
  src/h264_annexb.c
  src/input_sdl.c
  src/cursor_sdl.c
  src/ui_mixer.c
)

find_package(Threads REQUIRED)

target_include_directories(gnomecast-native PRIVATE include)
target_compile_definitions(gnomecast-native PRIVATE
  $<$<BOOL:${HELLOLG_TARGET_WEBOS}>:HELLOLG_TARGET_WEBOS=1>
  $<$<BOOL:${HELLOLG_WITH_SS4S}>:HELLOLG_WITH_SS4S=1>
  $<$<BOOL:${HELLOLG_WITH_SDL}>:HELLOLG_WITH_SDL=1>
  $<$<BOOL:${HELLOLG_WITH_PRECONNECT_UI}>:HELLOLG_WITH_PRECONNECT_UI=1>
)

# libm: the volume overlay's meter math (log10f) lives in SDL builds, but linking m
# unconditionally is harmless and keeps the host build in sync.
target_link_libraries(gnomecast-native PRIVATE Threads::Threads m)

if(HELLOLG_WITH_LS2)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
  target_compile_definitions(gnomecast-native PRIVATE HELLOLG_WITH_LS2=1)
  target_link_libraries(gnomecast-native PRIVATE PkgConfig::GLIB2 luna-service2)
endif()

if(HELLOLG_WITH_OPUS)
  # Reuses ss4s's recipe: builds libopus 1.4 from source under the active toolchain.
  # Placed BEFORE the ss4s subdirectory so its ndl-webos5 module (opus-fix shim) picks up
  # the same OPUS_FOUND/OPUS_LIBRARIES instead of creating a second ext_opus target.
  list(APPEND CMAKE_MODULE_PATH "${HELLOLG_SS4S_SOURCE_DIR}/cmake")
  include(ExternalOPUS)
  target_compile_definitions(gnomecast-native PRIVATE HELLOLG_WITH_OPUS=1)
  target_include_directories(gnomecast-native SYSTEM PRIVATE ${OPUS_INCLUDE_DIRS})
  target_link_libraries(gnomecast-native PRIVATE ${OPUS_LIBRARIES})
  ExternalProject_Get_Property(ext_opus INSTALL_DIR)
  set_property(TARGET gnomecast-native APPEND PROPERTY BUILD_RPATH "${INSTALL_DIR}/lib")
  # Ship our own copy: webOS carries a system libopus.so.0, but do not depend on it.
  install(DIRECTORY "${INSTALL_DIR}/lib/" DESTINATION ${CMAKE_INSTALL_LIBDIR}
    FILES_MATCHING PATTERN "libopus.so*")
endif()

if(HELLOLG_LINK_RDP_FFI)
  if(NOT DEFINED RDP_FFI_LIB)
    message(FATAL_ERROR "HELLOLG_LINK_RDP_FFI=ON requires -DRDP_FFI_LIB=/path/to/libwebrdp_min.a")
  endif()
  if(NOT EXISTS "${RDP_FFI_LIB}")
    message(FATAL_ERROR "RDP_FFI_LIB does not exist: ${RDP_FFI_LIB}")
  endif()
  target_link_libraries(gnomecast-native PRIVATE "${RDP_FFI_LIB}" Threads::Threads m dl)
else()
  target_sources(gnomecast-native PRIVATE src/rdp_ffi_stub.c)
endif()

if(HELLOLG_WITH_SDL)
  find_package(SDL2 REQUIRED)
  if(TARGET SDL2::SDL2)
    target_link_libraries(gnomecast-native PRIVATE SDL2::SDL2)
  else()
    target_include_directories(gnomecast-native PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(gnomecast-native PRIVATE ${SDL2_LIBRARIES})
  endif()

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)
  if(HELLOLG_TARGET_WEBOS)
    find_library(HELLOLG_LIBEVDEV_STATIC_LIBRARY NAMES libevdev.a PATHS ${LIBEVDEV_LIBRARY_DIRS} NO_DEFAULT_PATH)
    if(NOT HELLOLG_LIBEVDEV_STATIC_LIBRARY)
      message(FATAL_ERROR "webOS SDL builds require static libevdev.a in the target sysroot")
    endif()
    target_include_directories(gnomecast-native PRIVATE ${LIBEVDEV_INCLUDE_DIRS})
    target_link_libraries(gnomecast-native PRIVATE "${HELLOLG_LIBEVDEV_STATIC_LIBRARY}" rt)
  elseif(TARGET PkgConfig::LIBEVDEV)
    target_link_libraries(gnomecast-native PRIVATE PkgConfig::LIBEVDEV)
  else()
    target_include_directories(gnomecast-native PRIVATE ${LIBEVDEV_INCLUDE_DIRS})
    target_link_libraries(gnomecast-native PRIVATE ${LIBEVDEV_LIBRARIES})
  endif()
  message(STATUS "Using libevdev for native mouse/keyboard readers")

  include(CheckIncludeFile)
  check_include_file("linux/input.h" HELLOLG_HAVE_LINUX_INPUT_H)
  if(NOT HELLOLG_HAVE_LINUX_INPUT_H)
    message(FATAL_ERROR "linux/input.h not found: libevdev input readers require Linux input event codes")
  endif()

  target_sources(gnomecast-native PRIVATE src/input_evdev.c)
  target_compile_definitions(gnomecast-native PRIVATE HELLOLG_WITH_EVDEV_MOUSE=1 HELLOLG_WITH_EVDEV_KEYBOARD=1)
endif()

if(HELLOLG_WITH_PRECONNECT_UI)
  if(NOT HELLOLG_WITH_SDL)
    message(FATAL_ERROR "HELLOLG_WITH_PRECONNECT_UI=ON requires HELLOLG_WITH_SDL=ON")
  endif()
  if(NOT EXISTS "${HELLOLG_LVGL_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "HELLOLG_WITH_PRECONNECT_UI=ON requires initialized LVGL at ${HELLOLG_LVGL_SOURCE_DIR}; run git submodule update --init third_party/lvgl")
  endif()

  set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/include/lv_conf.h" CACHE STRING "gnomecast LVGL config" FORCE)
  set(LV_CONF_INCLUDE_SIMPLE ON CACHE BOOL "Use simple lv_conf.h include" FORCE)
  set(LV_LVGL_H_INCLUDE_SIMPLE ON CACHE BOOL "Use simple lvgl.h include" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build LVGL static for native package" FORCE)
  add_subdirectory("${HELLOLG_LVGL_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/third_party/lvgl" EXCLUDE_FROM_ALL)
  target_sources(gnomecast-native PRIVATE src/ui_preconnect.c)
  target_include_directories(gnomecast-native PRIVATE "${HELLOLG_LVGL_SOURCE_DIR}/src")
  target_link_libraries(gnomecast-native PRIVATE lvgl)
  if(TARGET SDL2::SDL2)
    target_link_libraries(lvgl PUBLIC SDL2::SDL2)
  else()
    target_include_directories(lvgl SYSTEM PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
  endif()
endif()

if(HELLOLG_WITH_SS4S)
  if(NOT EXISTS "${HELLOLG_SS4S_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "HELLOLG_WITH_SS4S=ON requires initialized ss4s at ${HELLOLG_SS4S_SOURCE_DIR}; run git submodule update --init third_party/ss4s")
  endif()

  set(SS4S_ENABLE_TESTS OFF CACHE BOOL "Disable ss4s tests for native package builds" FORCE)
  set(SS4S_ENABLE_SAMPLES OFF CACHE BOOL "Disable ss4s samples for native package builds" FORCE)
  set(SS4S_MODULE_BUILD_DUMMY OFF CACHE BOOL "Dummy ss4s backend is not allowed for native MVP" FORCE)
  if(HELLOLG_TARGET_WEBOS)
    set(SS4S_GENERATE_MODULES_LIST ON CACHE BOOL "Generate the ss4s module list for packaged modules" FORCE)
  else()
    message(STATUS "Configuring ss4s core without module list because this is not a webOS toolchain")
    set(SS4S_GENERATE_MODULES_LIST OFF CACHE BOOL "Generate the ss4s module list for packaged modules" FORCE)
  endif()

  if(HELLOLG_SS4S_WEBOS_MODULES_ONLY)
    foreach(module IN ITEMS alsa pulse sdl mmal steamlink lgnc)
      string(TOUPPER "${module}" module_upper)
      set("SS4S_MODULE_DISABLE_${module_upper}" ON CACHE BOOL "Disable non-MVP ss4s module ${module}" FORCE)
    endforeach()
  endif()

  add_subdirectory("${HELLOLG_SS4S_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/third_party/ss4s" EXCLUDE_FROM_ALL)
  target_link_libraries(gnomecast-native PRIVATE ss4s)

  get_target_property(HELLOLG_SS4S_MODULE_LIBRARY_OUTPUT_DIRECTORY ss4s SS4S_MODULE_LIBRARY_OUTPUT_DIRECTORY)
  if(HELLOLG_SS4S_MODULE_LIBRARY_OUTPUT_DIRECTORY)
    set_property(TARGET gnomecast-native APPEND PROPERTY BUILD_RPATH "${HELLOLG_SS4S_MODULE_LIBRARY_OUTPUT_DIRECTORY}")
    install(DIRECTORY "${HELLOLG_SS4S_MODULE_LIBRARY_OUTPUT_DIRECTORY}/"
      DESTINATION ${CMAKE_INSTALL_LIBDIR}
      FILES_MATCHING
      PATTERN "ss4s-*.so*"
      PATTERN "ss4s_modules.ini"
    )
  endif()
  set_property(TARGET gnomecast-native APPEND PROPERTY INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS gnomecast-native RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES deploy/webos/appinfo.json DESTINATION .)
install(FILES deploy/webos/icon.png DESTINATION .)
install(FILES deploy/webos/splash.png DESTINATION .)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/PROVENANCE.md"
  DESTINATION licenses
  RENAME THIRD_PARTY_PROVENANCE.md
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/LICENSE-MIT")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/LICENSE-MIT" DESTINATION licenses RENAME IronRDP-MIT.txt)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/LICENSE-APACHE")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/LICENSE-APACHE" DESTINATION licenses RENAME IronRDP-APACHE-2.0.txt)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/PROVENANCE.md")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/IronRDP/PROVENANCE.md" DESTINATION licenses RENAME IronRDP_PROVENANCE.md)
endif()
if(EXISTS "${HELLOLG_SS4S_SOURCE_DIR}/LICENSE")
  install(FILES "${HELLOLG_SS4S_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME ss4s-LGPL-3.0.txt)
endif()
if(EXISTS "${HELLOLG_COMMONS_SOURCE_DIR}/LICENSE")
  install(FILES "${HELLOLG_COMMONS_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME commons-MIT.txt)
endif()
if(EXISTS "${HELLOLG_LVGL_SOURCE_DIR}/LICENCE.txt")
  install(FILES "${HELLOLG_LVGL_SOURCE_DIR}/LICENCE.txt" DESTINATION licenses RENAME lvgl-MIT.txt)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libevdev-COPYING")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libevdev-COPYING" DESTINATION licenses RENAME libevdev-COPYING.txt)
endif()

set(HELLOLG_NATIVE_STAGE_DIR "${CMAKE_BINARY_DIR}/webos-stage" CACHE PATH "Staging directory for native webOS packaging")
add_custom_target(webos-stage-native
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}" --prefix "${HELLOLG_NATIVE_STAGE_DIR}"
  DEPENDS gnomecast-native
  COMMENT "Stage native-only webOS package contents"
)
add_custom_target(webos-package-native
  COMMAND "${CMAKE_COMMAND}" -E env NATIVE_WEBOS_BUILD_DIR="${CMAKE_BINARY_DIR}" NATIVE_WEBOS_SKIP_BUILD=1 NATIVE_WEBOS_STAGING_DIR="${HELLOLG_NATIVE_STAGE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/build-native-webos.sh"
  DEPENDS webos-stage-native
  COMMENT "Package native webOS app with ares-package"
)
add_custom_target(webos-install-native
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../tools/deploy-native-webos.sh" --no-launch
  DEPENDS webos-package-native
  COMMENT "Install native webOS package with ares-install"
)
add_custom_target(webos-launch-native
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../tools/deploy-native-webos.sh" --no-install
  DEPENDS webos-install-native
  COMMENT "Launch native webOS app with ares-launch"
)

if(BUILD_TESTING)
  add_executable(test-rdp-ffi-layout
    tests/test_rdp_ffi_layout.c
  )
  target_include_directories(test-rdp-ffi-layout PRIVATE include)
  add_test(NAME rdp-ffi-layout COMMAND test-rdp-ffi-layout)

  add_executable(test-h264-annexb
    tests/test_h264_annexb.c
    src/h264_annexb.c
  )
  target_include_directories(test-h264-annexb PRIVATE include)
  add_test(NAME h264-annexb COMMAND test-h264-annexb)

  add_executable(test-video-rgba-sdl
    tests/test_video_rgba_sdl.c
    src/video_rgba_sdl.c
  )
  target_include_directories(test-video-rgba-sdl PRIVATE include)
  add_test(NAME video-rgba-sdl COMMAND test-video-rgba-sdl)

  add_executable(test-input-sdl
    tests/test_input_sdl.c
    src/input_sdl.c
  )
  target_include_directories(test-input-sdl PRIVATE include)
  add_test(NAME input-sdl COMMAND test-input-sdl)

  add_executable(test-cursor-sdl
    tests/test_cursor_sdl.c
    src/cursor_sdl.c
  )
  target_include_directories(test-cursor-sdl PRIVATE include)
  target_link_libraries(test-cursor-sdl PRIVATE Threads::Threads)
  add_test(NAME cursor-sdl COMMAND test-cursor-sdl)

  add_executable(test-config-paths
    tests/test_config_paths.c
    src/config_paths.c
  )
  target_include_directories(test-config-paths PRIVATE include)
  add_test(NAME config-paths COMMAND test-config-paths)

  add_executable(test-settings-json
    tests/test_settings_json.c
    src/settings_json.c
    src/config_paths.c
  )
  target_include_directories(test-settings-json PRIVATE include)
  add_test(NAME settings-json COMMAND test-settings-json)

  add_executable(test-au-snapshot
    tests/test_au_snapshot.c
    src/au_snapshot.c
  )
  target_include_directories(test-au-snapshot PRIVATE include)
  add_test(NAME au-snapshot COMMAND test-au-snapshot)

  add_executable(test-luna-volume
    tests/test_luna_volume.c
    src/luna_volume.c
  )
  target_include_directories(test-luna-volume PRIVATE include)
  target_link_libraries(test-luna-volume PRIVATE Threads::Threads)
  add_test(NAME luna-volume COMMAND test-luna-volume)

  add_executable(test-audio-mixer
    tests/test_audio_mixer.c
    src/audio_mixer.c
  )
  target_include_directories(test-audio-mixer PRIVATE include)
  target_link_libraries(test-audio-mixer PRIVATE Threads::Threads)
  add_test(NAME audio-mixer COMMAND test-audio-mixer)

  if(HELLOLG_WITH_OPUS)
    add_executable(test-audio-opus
      tests/test_audio_opus.c
      src/audio_opus.c
    )
    target_compile_definitions(test-audio-opus PRIVATE HELLOLG_WITH_OPUS=1)
    target_include_directories(test-audio-opus PRIVATE include)
    target_include_directories(test-audio-opus SYSTEM PRIVATE ${OPUS_INCLUDE_DIRS})
    target_link_libraries(test-audio-opus PRIVATE ${OPUS_LIBRARIES} m)
    set_property(TARGET test-audio-opus APPEND PROPERTY BUILD_RPATH "${INSTALL_DIR}/lib")
    add_test(NAME audio-opus COMMAND test-audio-opus)
  endif()
endif()
