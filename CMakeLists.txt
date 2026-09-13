cmake_minimum_required(VERSION 3.14)
project(PackageDownloaderLib LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nlohmann_json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(json)
endif()

# Find libcurl
find_package(CURL REQUIRED)

# Find lgx library (logos-package). Used post-download to verify a
# fetched .lgx against the catalog entry — see verifyDownloadAgainstIndex
# in package_downloader_lib.cpp.
if(NOT DEFINED LGX_ROOT)
    message(FATAL_ERROR "LGX_ROOT must be set to the logos-package library path")
endif()
message(STATUS "Using lgx library at: ${LGX_ROOT}")
# NO_CMAKE_FIND_ROOT_PATH: an iOS toolchain re-roots find_library at the SDK,
# so the absolute path named in PATHS is silently rewritten to
# <sdk>/nix/store/... and nothing is found. NO_DEFAULT_PATH does not turn
# re-rooting off; only this does.
find_library(LGX_LIB lgx PATHS ${LGX_ROOT}/lib NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
message(STATUS "Found lgx library: ${LGX_LIB}")

# Library sources
set(LIB_SOURCES
    src/package_downloader_lib.cpp
    src/package_downloader_lib.h
    src/progress_throttle.cpp
    src/progress_throttle.h
    src/lgpd.cpp
    src/lgpd.h
)

# Create the library. Static for a host that LINKS it into the app image
# rather than loading it (iOS, where Qt itself is static and an app may carry
# no dynamic library of its own); shared everywhere else, which is what every
# existing build still gets.
option(LGPD_STATIC_LIB "Build package_downloader_lib as a static archive" OFF)
set(LGPD_LIB_TYPE SHARED)
if(LGPD_STATIC_LIB)
    set(LGPD_LIB_TYPE STATIC)
endif()
add_library(package_downloader_lib ${LGPD_LIB_TYPE} ${LIB_SOURCES})

# The C API in src/lgpd.h is explicitly __declspec(dllexport)-ed, and GNU ld
# disables PE auto-export for the ENTIRE image as soon as any symbol is
# explicitly exported. The C++ classes the CLI links against carry no
# annotation, so they silently vanished from the export table and lgpm/lgpd
# failed to link with 42 undefined references to perfectly ordinary methods.
# Ask CMake to generate a .def covering everything else as well.
if(WIN32)
    # Ask ld directly rather than via WINDOWS_EXPORT_ALL_SYMBOLS. That property
    # generates a .def file, and under this cross toolchain CMake never emits
    # one -- no .def appears in the build and no objdump runs -- so it silently
    # exports nothing. (Supplying CMAKE_OBJDUMP, the usual cause, made no
    # difference; verified.) --export-all-symbols is ld's own switch and needs
    # none of that machinery.
    target_link_options(package_downloader_lib PRIVATE -Wl,--export-all-symbols)
endif()


# Export macro for shared library
if(NOT LGPD_STATIC_LIB)
    target_compile_definitions(package_downloader_lib PRIVATE LGPD_BUILD_SHARED=1)
endif()

set_target_properties(package_downloader_lib PROPERTIES
    OUTPUT_NAME "package_downloader_lib"
)

# Include directories
target_include_directories(package_downloader_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(package_downloader_lib PRIVATE
    ${LGX_ROOT}/include
)

# Link dependencies
target_link_libraries(package_downloader_lib PRIVATE
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ${LGX_LIB}
)

# RPATH settings
set_target_properties(package_downloader_lib PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH FALSE
)

# There is no dylib to rewrite under LGPD_STATIC_LIB, and install_name_tool
# refuses an archive.
if(APPLE AND NOT LGPD_STATIC_LIB)
    set_target_properties(package_downloader_lib PROPERTIES
        INSTALL_RPATH "@loader_path;${LGX_ROOT}/lib"
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE
    )
    add_custom_command(TARGET package_downloader_lib POST_BUILD
        COMMAND install_name_tool -id "@rpath/libpackage_downloader_lib.dylib" $<TARGET_FILE:package_downloader_lib>
        # Rewrite the absolute nix-store lgx path to @rpath so the
        # liblgx.dylib bundled next to us (see nix/lib.nix) resolves.
        COMMAND install_name_tool -change "${LGX_LIB}" "@rpath/liblgx.dylib" $<TARGET_FILE:package_downloader_lib>
        COMMENT "Updating library paths for macOS"
    )
else()
    set_target_properties(package_downloader_lib PROPERTIES
        INSTALL_RPATH "$ORIGIN:${LGX_ROOT}/lib"
        INSTALL_RPATH_USE_LINK_PATH FALSE
    )
endif()

# Set output directories
set_target_properties(package_downloader_lib PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# CLI executable. Not for iOS: an executable in the install tree is not
# something an iOS app bundle can hold.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(cmd)
endif()

# Testing (optional)
option(LGPD_BUILD_TESTS "Build tests" OFF)
if(LGPD_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

# Install rules
install(TARGETS package_downloader_lib
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES src/package_downloader_lib.h src/lgpd.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
