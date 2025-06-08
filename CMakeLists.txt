cmake_minimum_required(VERSION 3.15) # ExternalProject benefits from newer versions
project(BPlusTree)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Include ExternalProject module ---
include(ExternalProject)

# --- Define paths for liburing ---
set(LIBURING_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/liburing)
set(PREGENERATED_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pregen)

# --- liburing install paths (within the CMake build directory) ---
# IMPORTANT: These are defined AFTER project() so CMAKE_CURRENT_BINARY_DIR is set.
set(LIBURING_INSTALL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/liburing_install)
set(LIBURING_EP_INCLUDEDIR ${LIBURING_INSTALL_PREFIX}/include) # Dir where liburing's 'make install' will place headers
set(LIBURING_EP_LIBDIR ${LIBURING_INSTALL_PREFIX}/lib)         # Dir where liburing's 'make install' will place libraries

# Path for INTERFACE_INCLUDE_DIRECTORIES:
# liburing installs to <prefix>/include/liburing/liburing.h
# So, to use #include <liburing/liburing.h>, the include path is <prefix>/include
set(LIBURING_INTERFACE_INCLUDE_DIR ${LIBURING_EP_INCLUDEDIR})

# Expected location of the static library after ExternalProject install
set(LIBURING_STATIC_LIB_PATH ${LIBURING_EP_LIBDIR}/liburing.a)
set(LIBURING_EXPECTED_HEADER_FILE ${LIBURING_INTERFACE_INCLUDE_DIR}/liburing/liburing.h)


# --- Pre-create the directory for INTERFACE_INCLUDE_DIRECTORIES ---
# This directory MUST exist at CMake configure time.
message(STATUS "Ensuring liburing interface include directory exists: ${LIBURING_INTERFACE_INCLUDE_DIR}")
file(MAKE_DIRECTORY ${LIBURING_INTERFACE_INCLUDE_DIR})
# Also pre-create lib dir, though not strictly needed for this specific error
file(MAKE_DIRECTORY ${LIBURING_EP_LIBDIR})


# --- Copy pre-generated configuration files to liburing source tree ---
# This happens at CMake configure time.
if(NOT EXISTS "${PREGENERATED_CONFIG_DIR}/config-host.h")
    message(FATAL_ERROR "Pre-generated config-host.h not found at ${PREGENERATED_CONFIG_DIR}/config-host.h")
endif()
# ... (add similar checks for other pregenerated files if desired)

# Ensure target subdirectories in liburing source tree exist before copying
file(MAKE_DIRECTORY ${LIBURING_SOURCE_DIR}/src/include/liburing)

message(STATUS "Copying pre-generated config-host.h to ${LIBURING_SOURCE_DIR}/config-host.h")
configure_file(${PREGENERATED_CONFIG_DIR}/config-host.h
        ${LIBURING_SOURCE_DIR}/config-host.h
        COPYONLY)

message(STATUS "Copying pre-generated config-host.mak to ${LIBURING_SOURCE_DIR}/config-host.mak")
configure_file(${PREGENERATED_CONFIG_DIR}/config-host.mak
        ${LIBURING_SOURCE_DIR}/config-host.mak
        COPYONLY)

message(STATUS "Copying pre-generated io_uring_version.h to ${LIBURING_SOURCE_DIR}/src/include/liburing/io_uring_version.h")
configure_file(${PREGENERATED_CONFIG_DIR}/io_uring_version.h
        ${LIBURING_SOURCE_DIR}/src/include/liburing/io_uring_version.h
        COPYONLY)

message(STATUS "Copying pre-generated compat.h to ${LIBURING_SOURCE_DIR}/src/include/liburing/compat.h")
configure_file(${PREGENERATED_CONFIG_DIR}/compat.h
        ${LIBURING_SOURCE_DIR}/src/include/liburing/compat.h
        COPYONLY)


# --- Configure, build, and install liburing using ExternalProject_Add ---
set(LIBURING_PROJECT_NAME liburing_ep)

if(NOT EXISTS "${LIBURING_SOURCE_DIR}/Makefile") # Check for Makefile, not configure
    message(FATAL_ERROR "liburing Makefile not found at ${LIBURING_SOURCE_DIR}/Makefile. Please ensure liburing sources are correctly placed and pre-generated files are copied.")
endif()

ExternalProject_Add(${LIBURING_PROJECT_NAME}
        SOURCE_DIR          ${LIBURING_SOURCE_DIR}
        DOWNLOAD_COMMAND    ""
        UPDATE_COMMAND      ""
        PATCH_COMMAND       ""

        CONFIGURE_COMMAND   "" # We are using pre-generated config files

        # liburing's top Makefile typically calls 'make -C src'
        # Ensure CMAKE_MAKE_PROGRAM is used for portability
        BUILD_COMMAND       ${CMAKE_MAKE_PROGRAM} -C src CC=${CMAKE_C_COMPILER}
        # If liburing's main Makefile directly builds, then:
        # BUILD_COMMAND       ${CMAKE_MAKE_PROGRAM} CC=${CMAKE_C_COMPILER}


        # Correctly pass variables to 'make install'
        # These are arguments to the 'make' command, not separate EP_Add arguments
        INSTALL_COMMAND     ${CMAKE_MAKE_PROGRAM} -C src install # Or just ${CMAKE_MAKE_PROGRAM} install if top Makefile handles it
        prefix=${LIBURING_INSTALL_PREFIX}
        includedir=${LIBURING_EP_INCLUDEDIR}
        libdir=${LIBURING_EP_LIBDIR}
        libdevdir=${LIBURING_EP_LIBDIR} # Assuming libdevdir is same as libdir for simplicity
        # relativelibdir="" # Often not needed if paths are absolute
        # mandir=... datadir=... (can be omitted if not needed)

        # BUILD_IN_SOURCE: If liburing's Makefiles build directly in the source tree (common for non-CMake projects)
        # If 'make -C src' is used, BUILD_IN_SOURCE refers to whether EP creates a separate build dir for itself,
        # not whether liburing itself builds in-source. For liburing, its own Makefiles dictate its build location.
        # Setting BUILD_IN_SOURCE 1 means EP itself won't create a <projname>-build subdir.
        # This is fine if liburing's Makefiles are well-behaved and use $(LIBURING_SOURCE_DIR)
        BUILD_IN_SOURCE     1 # liburing's Makefiles operate in its source directory

        # BYPRODUCTS tell CMake which files to expect after the INSTALL_COMMAND completes.
        # These paths are relative to the build tree if not absolute.
        # Using absolute paths based on LIBURING_INSTALL_PREFIX is clearer.
        BYPRODUCTS          ${LIBURING_STATIC_LIB_PATH}
        ${LIBURING_EXPECTED_HEADER_FILE}
        LOG_BUILD           1
        LOG_INSTALL         1
)

# --- Create an IMPORTED library target for liburing ---
# This target represents the liburing built by ExternalProject_Add.
add_library(uring_custom_built STATIC IMPORTED GLOBAL)

# Tell CMake where to find the library file and include directories.
# IMPORTED_LOCATION is the path to the actual .a or .so file.
set_property(TARGET uring_custom_built PROPERTY IMPORTED_LOCATION ${LIBURING_STATIC_LIB_PATH})

# INTERFACE_INCLUDE_DIRECTORIES is the directory to add to the include path
# for targets that link against uring_custom_built.
# This directory MUST exist at CMake configure time. We created it with file(MAKE_DIRECTORY).
set_property(TARGET uring_custom_built PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${LIBURING_INTERFACE_INCLUDE_DIR})

# Make sure uring_custom_built depends on the ExternalProject completing.
add_dependencies(uring_custom_built ${LIBURING_PROJECT_NAME})


# --- Your Project's Targets ---
include_directories(.) # For headers in the root of your project
include_directories(include) # For headers in your project's 'include' directory

# Example of finding your own source files
file(GLOB_RECURSE MAIN_APP_SOURCES "src/*.cpp")
file(GLOB_RECURSE INCLUDE_IMPL_SOURCES "include/*.cpp") # If you have .cpp files in include

add_executable(code ${MAIN_APP_SOURCES} ${INCLUDE_IMPL_SOURCES})

# Link your executable against the imported liburing target.
# add_dependencies is not strictly needed here because uring_custom_built already
# depends on LIBURING_PROJECT_NAME, and 'code' depends on uring_custom_built.
target_link_libraries(code PRIVATE uring_custom_built)
target_compile_options(code PRIVATE -O2) # Or whatever options you need

message(STATUS "Configured 'code' executable, linking against 'uring_custom_built'.")
message(STATUS "  liburing source: ${LIBURING_SOURCE_DIR}")
message(STATUS "  liburing install prefix (EP): ${LIBURING_INSTALL_PREFIX}")
message(STATUS "  liburing static library (expected): ${LIBURING_STATIC_LIB_PATH}")
message(STATUS "  liburing interface include dir: ${LIBURING_INTERFACE_INCLUDE_DIR}")