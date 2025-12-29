cmake_minimum_required(VERSION 3.22)
project(uvccamera-native-tests)

# Host-only build for testing
message(STATUS "Building UVCCamera native tests for host")

# Enable testing
enable_testing()

# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Test compilation flags - MUST come before source files
add_compile_definitions(UVCCAMERA_TESTING=1)

# Include paths
set(PRODUCTION_SRC ${CMAKE_SOURCE_DIR}/../UVCCamera)

# Test executable - includes production code with mock guards
add_executable(native_tests
    # Production code under test (with UVCCAMERA_TESTING guards)
    ${PRODUCTION_SRC}/FrameBufferRing.cpp

    # Test files
    tests/ContractTest.cpp
    tests/StreamTelemetryTest.cpp
    tests/FrameBufferRingTest.cpp

    # Mocks
    mocks/AndroidApiMocks.cpp
)

target_include_directories(native_tests PRIVATE
    include
    mocks
    ${PRODUCTION_SRC}
)

target_link_libraries(native_tests
    GTest::gtest_main
    GTest::gmock
)

# Discover tests for CTest
include(GoogleTest)
gtest_discover_tests(native_tests)
