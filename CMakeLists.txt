cmake_minimum_required(VERSION 3.28)
project(BPlusTree)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON) # Good practice
set(CMAKE_CXX_EXTENSIONS OFF)       # Good practice, avoid GNU extensions by default

include_directories(.)
include_directories(include)
include_directories(thirdparty/liburing)

file(GLOB_RECURSE INCLUDE_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/*.cpp")
if(INCLUDE_SOURCE_FILES)
    message(STATUS "Found include source files: ${INCLUDE_SOURCE_FILES}")
else()
    message(WARNING "No implementation source files (*.cpp) found in ${CMAKE_CURRENT_SOURCE_DIR}/include. Linker errors for non-inline header methods might occur if implementations are not in headers.")
endif()


add_executable(code src.cpp ${INCLUDE_SOURCE_FILES})
# Link liburing to the 'code' executable
target_link_libraries(code PRIVATE uring)
# Set -O2 optimization specifically for the 'code' executable
target_compile_options(code PRIVATE -O2)
message(STATUS "Configuring 'code' executable with -O2 and linking against liburing.")

enable_testing()
include(CTest)

# Source files from the 'include' directory that provide implementations
# (like disk_scheduler.cpp)


set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/local-test)
file(GLOB_RECURSE TEST_SOURCE_FILES "${TEST_DIR}/*.cpp")

if(TEST_SOURCE_FILES)
    message(STATUS "Found test source files: ${TEST_SOURCE_FILES}")
    foreach(TEST_SOURCE_FILE ${TEST_SOURCE_FILES})
        get_filename_component(TEST_NAME ${TEST_SOURCE_FILE} NAME_WE)

        # Create an executable target for this specific test file
        # Add both the test file AND the implementation files from 'include'
        add_executable(${TEST_NAME}_test ${TEST_SOURCE_FILE} ${INCLUDE_SOURCE_FILES})

        # Link liburing to this specific test executable
        target_link_libraries(${TEST_NAME}_test PRIVATE uring)

        # Set -O0 optimization specifically for this test executable
        target_compile_options(${TEST_NAME}_test PRIVATE -O0)

        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME}_test)
        message(STATUS "Added test: ${TEST_NAME} (Executable: ${TEST_NAME}_test, Sources: ${TEST_SOURCE_FILE} ${INCLUDE_SOURCE_FILES}, Opt: -O0, Links: uring)")
    endforeach()
else()
    message(WARNING "No test source files (*.cpp) found in ${TEST_DIR}")
endif()