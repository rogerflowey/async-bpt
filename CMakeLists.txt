cmake_minimum_required(VERSION 3.28)
project(BPlusTree)

set(CMAKE_CXX_STANDARD 20)

include_directories(.)
include_directories(include)

add_executable(code src.cpp)

# Unit Tests
enable_testing()
include(CTest)
set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/local-test)
file(GLOB TEST_SOURCE_FILES "${TEST_DIR}/*.cpp")
if(TEST_SOURCE_FILES)
    message(STATUS "Found test source files: ${TEST_SOURCE_FILES}")
    foreach(TEST_SOURCE_FILE ${TEST_SOURCE_FILES})
        # Extract the filename without the directory path and extension
        # This will be used as the executable name and the test name
        get_filename_component(TEST_NAME ${TEST_SOURCE_FILE} NAME_WE) # NAME_WE = Name Without Extension

        # Create an executable target for this specific test file
        add_executable(${TEST_NAME}_test ${TEST_SOURCE_FILE})
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME}_test)
        message(STATUS "Added test: ${TEST_NAME} (Executable: ${TEST_NAME}_test)")
    endforeach()
else()
    message(WARNING "No test source files (*.cpp) found in ${TEST_DIR}")
endif()