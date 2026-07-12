cmake_minimum_required(VERSION 3.19)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(backend_ndl VERSION 0.1.0 LANGUAGES C)
  include(CTest)
  set(BACKEND_NDL_TOP_LEVEL ON)
else()
  set(BACKEND_NDL_TOP_LEVEL OFF)
endif()

include(GNUInstallDirs)

option(BACKEND_NDL_ENABLE "Compile the webOS DirectMedia runtime implementation" OFF)
option(BACKEND_NDL_BUILD_TESTS "Build backend_ndl host tests" ${BUILD_TESTING})
option(BACKEND_NDL_INSTALL "Install backend_ndl development files" ${BACKEND_NDL_TOP_LEVEL})
set(BACKEND_NDL_NDK_INCLUDE_DIRS "" CACHE STRING
    "Optional DirectMedia SDK include directories; pkg-config is used when empty")

add_library(backend_ndl STATIC src/backend_ndl.c)
add_library(backend_ndl::backend_ndl ALIAS backend_ndl)
set_target_properties(backend_ndl PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS YES
  POSITION_INDEPENDENT_CODE YES
  EXPORT_NAME backend_ndl
)
target_include_directories(backend_ndl
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE src
)

if(BACKEND_NDL_ENABLE)
  find_package(Threads REQUIRED)
  if(BACKEND_NDL_NDK_INCLUDE_DIRS)
    set(BACKEND_NDL_PLATFORM_INCLUDE_DIRS ${BACKEND_NDL_NDK_INCLUDE_DIRS})
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BACKEND_NDL_NDK REQUIRED NDL_directmedia)
    set(BACKEND_NDL_PLATFORM_INCLUDE_DIRS ${BACKEND_NDL_NDK_INCLUDE_DIRS})
    target_compile_options(backend_ndl PRIVATE ${BACKEND_NDL_NDK_CFLAGS_OTHER})
  endif()
  target_compile_definitions(backend_ndl PRIVATE BACKEND_NDL_ENABLED=1)
  target_include_directories(backend_ndl SYSTEM PRIVATE ${BACKEND_NDL_PLATFORM_INCLUDE_DIRS})
  target_link_libraries(backend_ndl PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  if(UNIX AND NOT APPLE)
    target_link_libraries(backend_ndl PRIVATE rt)
  endif()
endif()

if(BACKEND_NDL_BUILD_TESTS)
  find_package(Threads REQUIRED)
  set(BACKEND_NDL_MOCK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/libndl-media")

  add_library(backend_ndl_test_support STATIC src/backend_ndl.c)
  add_library(backend_ndl::test_support ALIAS backend_ndl_test_support)
  set_target_properties(backend_ndl_test_support PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
  )
  target_compile_definitions(backend_ndl_test_support PUBLIC
    BACKEND_NDL_ENABLED=1
    BACKEND_NDL_TESTING=1
  )
  target_include_directories(backend_ndl_test_support
    PUBLIC include src
    SYSTEM PUBLIC "${BACKEND_NDL_MOCK_INCLUDE_DIR}"
  )
  target_link_libraries(backend_ndl_test_support PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  if(UNIX AND NOT APPLE)
    target_link_libraries(backend_ndl_test_support PUBLIC rt)
  endif()

  add_executable(test-backend-ndl tests/test_backend_ndl.c)
  target_link_libraries(test-backend-ndl PRIVATE backend_ndl::test_support)
  add_test(NAME backend-ndl COMMAND test-backend-ndl)
endif()

if(BACKEND_NDL_INSTALL)
  include(CMakePackageConfigHelpers)
  install(TARGETS backend_ndl EXPORT backend_ndlTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
  install(DIRECTORY include/backend_ndl DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(FILES LICENSE README.md THIRD_PARTY.md DESTINATION "${CMAKE_INSTALL_DATADIR}/backend_ndl")
  install(EXPORT backend_ndlTargets
    FILE backend_ndlTargets.cmake
    NAMESPACE backend_ndl::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/backend_ndl"
  )
  configure_package_config_file(
    cmake/backend_ndlConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/backend_ndlConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/backend_ndl"
  )
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/backend_ndlConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/backend_ndlConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/backend_ndlConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/backend_ndl"
  )
endif()
