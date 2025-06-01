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


# --- Pre-create directories to satisfy CMake's configure-time checks ---
message(STATUS "Pre-creating install directories for liburing:")
message(STATUS "  Include base dir: ${LIBURING_INTERFACE_INCLUDE_PATH}") # This is .../liburing_install/include
message(STATUS "  Lib dir: ${LIBURING_CONFIGURE_LIBDIR_ARG}")
# liburing's 'make install' creates '.../include/liburing', so '.../include' must exist.
file(MAKE_DIRECTORY ${LIBURING_INTERFACE_INCLUDE_PATH})
file(MAKE_DIRECTORY ${LIBURING_CONFIGURE_LIBDIR_ARG})
# It's also good practice to ensure the specific subdirectory for headers is also pre-created
# if you are very specific with INTERFACE_INCLUDE_DIRECTORIES, but since liburing's make install
# uses `install -d $(includedir)/liburing`, it will create the 'liburing' subdirectory itself
# as long as $(includedir) exists.

# --- Configure, build, and install liburing using ExternalProject_Add ---
message(STATUS "Configuring ExternalProject for liburing")
message(STATUS "  Source dir: ${LIBURING_SOURCE_DIR}")
message(STATUS "  Install prefix: ${LIBURING_CONFIGURE_PREFIX_ARG}")

if(NOT EXISTS "${LIBURING_SOURCE_DIR}/configure")
    message(FATAL_ERROR "liburing configure script not found at ${LIBURING_SOURCE_DIR}/configure. Please ensure liburing sources are correctly placed.")
endif()

ExternalProject_Add(${LIBURING_PROJECT_NAME}
        SOURCE_DIR          ${LIBURING_SOURCE_DIR}
        DOWNLOAD_COMMAND    ""
        UPDATE_COMMAND      ""

        # Skip configure command or make it a no-op
        CONFIGURE_COMMAND   ""
        # Or CONFIGURE_COMMAND echo "Skipping configure; using pre-generated Makefile"

        # Build command would now directly use the (hopefully) existing Makefile
        # It needs to be told where to install things, as 'configure' usually sets this in the Makefile
        BUILD_COMMAND       $(MAKE) # This might need to be $(MAKE) -C ${LIBURING_SOURCE_DIR}/src if the main lib is there

        # Install command also needs overrides
        INSTALL_COMMAND     $(MAKE) install  # No backslash
        prefix=${LIBURING_CONFIGURE_PREFIX_ARG}  # No backslash
        libdir=${LIBURING_CONFIGURE_LIBDIR_ARG}  # No backslash
        includedir=${LIBURING_CONFIGURE_INCLUDEDIR_ARG} # Last line, no backslash needed
        # You might need -C ${LIBURING_SOURCE_DIR}/src here too

        BUILD_BYPRODUCTS    ${LIBURING_STATIC_LIB_PATH} ${LIBURING_EXPECTED_HEADER_FILE}
        LOG_BUILD           1
        LOG_INSTALL         1
        BUILD_IN_SOURCE     1 # Likely needed if Makefiles are in the source dir
)

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