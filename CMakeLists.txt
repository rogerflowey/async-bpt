# --- Flexible CMakeLists.txt ---

cmake_minimum_required(VERSION 3.15)
project(BPlusTree)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Build Option ---
# Add an option to switch between the header-only version and the system library.
# You can change this with: cmake -DUSE_SYSTEM_LIBURING=ON ..
option(USE_SYSTEM_LIBURING "Use the installed system liburing instead of mini_uring.h" OFF)

# --- Common Setup ---

# Add include directories for your project's headers.
include_directories(
        .
        include
)

# Create a STATIC library from your application's core logic.
file(GLOB_RECURSE APP_SOURCES "src/*.cpp" "include/*.cpp")
add_library(app_core STATIC ${APP_SOURCES})

# --- Conditional Dependency Handling ---

if(USE_SYSTEM_LIBURING)
    # --- SCENARIO 1: Use the system's liburing library ---
    message(STATUS "Using system-installed liburing library.")

    # Link app_core against the found liburing library.
    # PUBLIC means that any target that links to app_core (like your tests)
    # will also be automatically linked against uring::uring. This is key.
    target_link_libraries(app_core PUBLIC uring)

else()
    # --- SCENARIO 2: Use the header-only mini_uring.h ---
    message(STATUS "Using header-only mini_uring.h.")

    # Define the location of our header-only io_uring implementation.
    set(MINI_URING_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)

    # The library needs access to mini_uring.h.
    # PUBLIC means any target linking to app_core will also get this include path.
    target_include_directories(app_core PUBLIC ${MINI_URING_INCLUDE_DIR})
    # No linking is needed in this case.
endif()


# --- Main Executable Target ---
add_executable(code src.cpp)

target_link_libraries(code PRIVATE app_core)
target_compile_options(code PRIVATE -O2)
#target_compile_options(code PRIVATE  -Wall -Wextra)

add_executable(code_test src.cpp)

target_link_libraries(code_test PRIVATE app_core)
target_compile_options(code_test PRIVATE -O2 -DCODE_TEST)
#target_compile_options(code PRIVATE  -Wall -Wextra)

set_target_properties(code_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/test_env"
)

message(STATUS "Configured main executable 'code'.")

add_executable(gen gen.cpp)
set_target_properties(gen PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/test_env"
)


# ==============================================================================
# --- Test Building Section ---
# ==============================================================================

#file(GLOB TEST_FILES "local-test/*.cpp" "local-test/abpt/*.cpp")
#message(STATUS "Found test files: ${TEST_FILES}")
#
#foreach(TEST_FILE ${TEST_FILES})
#    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
#    add_executable(${TEST_NAME} ${TEST_FILE})
#
#    # This is the crucial part. Because app_core has a PUBLIC dependency on
#    # either the include path (for mini_uring) or the link library (for system uring),
#    # this single line works for both scenarios.
#    target_link_libraries(${TEST_NAME} PRIVATE app_core)
#
#    set_target_properties(${TEST_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
#    message(STATUS "Configured test executable: ${TEST_NAME}")
#endforeach()
#
#message(STATUS "Build configuration complete.")