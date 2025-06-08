cmake_minimum_required(VERSION 3.28)
project(BPlusTree)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Include ExternalProject module ---
include(ExternalProject)

# --- Define paths for liburing ---
set(LIBURING_PROJECT_NAME liburing_ep) # Name for the ExternalProject target
set(LIBURING_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/liburing)

# Base directory where liburing will be "installed" by its own make install
set(LIBURING_INSTALL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/liburing_install)

# Paths passed to liburing's ./configure script
set(LIBURING_CONFIGURE_PREFIX_ARG ${LIBURING_INSTALL_PREFIX})
set(LIBURING_CONFIGURE_INCLUDEDIR_ARG ${LIBURING_INSTALL_PREFIX}/include) # liburing will create a 'liburing' subdir under this
set(LIBURING_CONFIGURE_LIBDIR_ARG ${LIBURING_INSTALL_PREFIX}/lib)

# Expected location of the compiled static library
set(LIBURING_STATIC_LIB_PATH ${LIBURING_CONFIGURE_LIBDIR_ARG}/liburing.a)

# Expected location of the header files for the INTERFACE_INCLUDE_DIRECTORIES property.
# If liburing installs to 'PREFIX/include/liburing/liburing.h',
# and you use #include <liburing/liburing.h>, then this should be 'PREFIX/include'.
# If you use #include <liburing.h>', then this should be 'PREFIX/include/liburing'.
# The error message used '.../liburing_install/include', so we'll stick with that,
# implying your code uses #include <liburing/liburing.h>.
set(LIBURING_INTERFACE_INCLUDE_PATH ${LIBURING_CONFIGURE_INCLUDEDIR_ARG})
# The actual header file that should exist after installation
set(LIBURING_EXPECTED_HEADER_FILE ${LIBURING_INTERFACE_INCLUDE_PATH}/liburing/liburing.h)

# ... (your CMake setup) ...

set(LIBURING_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/liburing)
set(PREGENERATED_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pregen) # Or wherever you put them

# Ensure target directories exist in liburing source tree
file(MAKE_DIRECTORY ${LIBURING_SOURCE_DIR}/src/include/liburing)

# Copy pre-generated files
# These commands will run at CMake configure time
configure_file(${PREGENERATED_CONFIG_DIR}/config-host.h ${LIBURING_SOURCE_DIR}/config-host.h COPYONLY)
configure_file(${PREGENERATED_CONFIG_DIR}/config-host.mak ${LIBURING_SOURCE_DIR}/config-host.mak COPYONLY)
configure_file(${PREGENERATED_CONFIG_DIR}/io_uring_version.h ${LIBURING_SOURCE_DIR}/src/include/liburing/io_uring_version.h COPYONLY)
configure_file(${PREGENERATED_CONFIG_DIR}/compat.h ${LIBURING_SOURCE_DIR}/src/include/liburing/compat.h COPYONLY)

# --- ExternalProject_Add for liburing ---
# ... (as before, but CONFIGURE_COMMAND is definitely "" or removed) ...

ExternalProject_Add(${LIBURING_PROJECT_NAME}
        SOURCE_DIR          ${LIBURING_SOURCE_DIR}
        DOWNLOAD_COMMAND    ""
        UPDATE_COMMAND      ""
        PATCH_COMMAND       ""

        CONFIGURE_COMMAND   "" # Crucial: Do not run liburing's configure

        # Build command: liburing's top Makefile calls make -C src
        # It should now use the pre-generated config-host.mak
        BUILD_COMMAND       $(MAKE) # Or $(MAKE) -C ${LIBURING_SOURCE_DIR} if top Makefile is simple
        # Or $(MAKE) -C ${LIBURING_SOURCE_DIR}/src if that's where the main lib is built
        # You might need to pass CC/CXX if they are not picked up correctly
        # e.g., $(MAKE) CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER}

        # Install command needs to use the paths you want for your CMake build
        INSTALL_COMMAND     $(MAKE) install
        prefix=${LIBURING_INSTALL_PREFIX}
        includedir=${LIBURING_INSTALL_PREFIX}/include # Corresponds to your LIBURING_CONFIGURE_INCLUDEDIR_ARG
        libdir=${LIBURING_INSTALL_PREFIX}/lib         # Corresponds to your LIBURING_CONFIGURE_LIBDIR_ARG
        libdevdir=${LIBURING_INSTALL_PREFIX}/lib      # Adjust if different
        # relativelibdir might not be needed if prefix/libdir/includedir are absolute and correct
        # mandir and datadir can be omitted if you don't need them installed by EP
        # mandir=${LIBURING_INSTALL_PREFIX}/man
        # datadir=${LIBURING_INSTALL_PREFIX}/share

        BUILD_IN_SOURCE     1 # Make commands run from LIBURING_SOURCE_DIR

        BYPRODUCTS          "${LIBURING_INSTALL_PREFIX}/lib/liburing.a"
        "${LIBURING_INSTALL_PREFIX}/include/liburing/liburing.h"
        LOG_BUILD           1
        LOG_INSTALL         1
)

# ... (rest of your CMakeLists.txt: add_library IMPORTED, etc.)

# --- Create an IMPORTED library target for liburing ---
message(STATUS "Creating imported target 'uring_custom_built' for ${LIBURING_STATIC_LIB_PATH}")
add_library(uring_custom_built STATIC IMPORTED GLOBAL)

set_property(TARGET uring_custom_built PROPERTY IMPORTED_LOCATION ${LIBURING_STATIC_LIB_PATH})
# This path should be what you add to -I for the compiler.
# If headers are at 'PREFIX/include/liburing/liburing.h' and you use '#include <liburing/liburing.h>',
# then this should be 'PREFIX/include'.
set_property(TARGET uring_custom_built PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${LIBURING_INTERFACE_INCLUDE_PATH})

# --- Your Project's Include Directories ---
include_directories(.)
include_directories(include)

file(GLOB_RECURSE INCLUDE_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/*.cpp")
if(INCLUDE_SOURCE_FILES)
    message(STATUS "Found include source files: ${INCLUDE_SOURCE_FILES}")
else()
    message(WARNING "No implementation source files (*.cpp) found in ${CMAKE_CURRENT_SOURCE_DIR}/include.")
endif()

add_executable(code src.cpp ${INCLUDE_SOURCE_FILES})
add_dependencies(code ${LIBURING_PROJECT_NAME}) # Ensure liburing is built first
target_link_libraries(code PRIVATE uring_custom_built)
target_compile_options(code PRIVATE -O2)
message(STATUS "Configuring 'code' executable with -O2 and linking against 'uring_custom_built'.")

enable_testing()
include(CTest)

set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/local-test)
file(GLOB_RECURSE TEST_SOURCE_FILES "${TEST_DIR}/*.cpp")

if(TEST_SOURCE_FILES)
    message(STATUS "Found test source files: ${TEST_SOURCE_FILES}")
    foreach(TEST_SOURCE_FILE ${TEST_SOURCE_FILES})
        get_filename_component(TEST_NAME ${TEST_SOURCE_FILE} NAME_WE)
        add_executable(${TEST_NAME}_test ${TEST_SOURCE_FILE} ${INCLUDE_SOURCE_FILES})
        add_dependencies(${TEST_NAME}_test ${LIBURING_PROJECT_NAME}) # Ensure liburing is built first
        target_link_libraries(${TEST_NAME}_test PRIVATE uring_custom_built)
        target_compile_options(${TEST_NAME}_test PRIVATE -O0)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME}_test)
        message(STATUS "Added test: ${TEST_NAME} (Executable: ${TEST_NAME}_test, Links: uring_custom_built)")
    endforeach()
else()
    message(WARNING "No test source files (*.cpp) found in ${TEST_DIR}")
endif()