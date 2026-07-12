# Builds the pinned libopus from source under the active toolchain and exposes
# it as OPUS_FOUND / OPUS_INCLUDE_DIRS / OPUS_LIBRARIES plus the `ext_opus`
# ExternalProject (the consumer reads its INSTALL_DIR to stage libopus.so into
# the package). Packaging stays with the HELLOLG_WITH_OPUS block in
# native/CMakeLists.txt, which installs only the libopus.so* pattern and the
# license notice.

# Tarball timestamps: keep extraction deterministic on CMake >= 3.24.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

include(ExternalProject)

# The webOS buildroot GCC may lack NEON fp16 support; opus intrinsics fail to
# build there. Probe the compiler banner instead of hardcoding a toolchain list.
set(HELLOLG_OPUS_DISABLE_INTRINSICS OFF)
get_filename_component(HELLOLG_OPUS_CC_NAME "${CMAKE_C_COMPILER}" NAME)
if(HELLOLG_OPUS_CC_NAME MATCHES "arm-webos-linux-gnueabi-.*")
  execute_process(COMMAND ${CMAKE_C_COMPILER} -v ERROR_VARIABLE HELLOLG_OPUS_CC_INFO OUTPUT_QUIET)
  if(NOT HELLOLG_OPUS_CC_INFO MATCHES "--with-fpu=neon-fp16")
    message(STATUS "opus: disabling intrinsics (toolchain lacks NEON fp16)")
    set(HELLOLG_OPUS_DISABLE_INTRINSICS ON)
  endif()
endif()

set(HELLOLG_OPUS_TOOLCHAIN_ARGS)
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND HELLOLG_OPUS_TOOLCHAIN_ARGS "-DCMAKE_TOOLCHAIN_FILE:string=${CMAKE_TOOLCHAIN_FILE}")
endif()

set(HELLOLG_OPUS_LIB_FILENAME "${CMAKE_SHARED_LIBRARY_PREFIX}opus${CMAKE_SHARED_LIBRARY_SUFFIX}")

ExternalProject_Add(ext_opus
  URL https://downloads.xiph.org/releases/opus/opus-1.4.tar.gz
  URL_HASH SHA256=c9b32b4253be5ae63d1ff16eea06b94b5f0f2951b7a02aceef58e3a3ce49c51f
  CMAKE_ARGS ${HELLOLG_OPUS_TOOLCHAIN_ARGS}
    -DCMAKE_BUILD_TYPE:string=${CMAKE_BUILD_TYPE}
    -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    -DOPUS_BUILD_SHARED_LIBRARY=ON
    -DOPUS_DISABLE_INTRINSICS=${HELLOLG_OPUS_DISABLE_INTRINSICS}
    -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF
    -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/${HELLOLG_OPUS_LIB_FILENAME}
)
ExternalProject_Get_Property(ext_opus INSTALL_DIR)

add_library(ext_opus_target SHARED IMPORTED)
set_target_properties(ext_opus_target PROPERTIES
  IMPORTED_LOCATION "${INSTALL_DIR}/lib/${HELLOLG_OPUS_LIB_FILENAME}")
add_dependencies(ext_opus_target ext_opus)

set(OPUS_INCLUDE_DIRS "${INSTALL_DIR}/include/opus")
set(OPUS_LIBRARIES ext_opus_target)
set(OPUS_FOUND TRUE)
