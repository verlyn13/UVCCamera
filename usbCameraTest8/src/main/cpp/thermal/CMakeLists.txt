cmake_minimum_required(VERSION 3.18.1)
project(thermal_xu)

# Define the thermal_xu library
add_library(thermal_xu SHARED
    thermal_xu.cpp
    extension_unit_discovery.cpp
)

# Check if prebuilt libraries exist for this ABI
set(PREBUILT_LIB_DIR "${CMAKE_SOURCE_DIR}/../../jniLibs/${ANDROID_ABI}")
set(UVCCAMERA_LIB "${PREBUILT_LIB_DIR}/libUVCCamera.so")
set(UVC_LIB "${PREBUILT_LIB_DIR}/libuvc.so")
set(USB100_LIB "${PREBUILT_LIB_DIR}/libusb100.so")

# Only import libraries if they exist for this ABI
if(EXISTS "${UVCCAMERA_LIB}" AND EXISTS "${UVC_LIB}" AND EXISTS "${USB100_LIB}")
    # Import the prebuilt UVCCamera library
    add_library(UVCCamera SHARED IMPORTED)
    set_target_properties(UVCCamera PROPERTIES
        IMPORTED_LOCATION "${UVCCAMERA_LIB}"
    )

    # Import the prebuilt libuvc library
    add_library(uvc SHARED IMPORTED)
    set_target_properties(uvc PROPERTIES
        IMPORTED_LOCATION "${UVC_LIB}"
    )

    # Import the prebuilt libusb library
    add_library(usb100 SHARED IMPORTED)
    set_target_properties(usb100 PROPERTIES
        IMPORTED_LOCATION "${USB100_LIB}"
    )
    
    set(HAVE_UVC_LIBS TRUE)
else()
    message(WARNING "UVC libraries not found for ${ANDROID_ABI}, building stub implementation")
    set(HAVE_UVC_LIBS FALSE)
endif()

# Include directories for headers
target_include_directories(thermal_xu PRIVATE
    ${CMAKE_SOURCE_DIR}/../../../../../lib/src/main/jni/libuvc/include
    ${CMAKE_SOURCE_DIR}/../../../../../lib/src/main/jni/UVCCamera
    ${CMAKE_SOURCE_DIR}/../../../../../lib/src/main/jni/libusb
)

# Link libraries conditionally
if(HAVE_UVC_LIBS)
    target_link_libraries(thermal_xu
        UVCCamera
        uvc
        usb100
        log
        android
    )
    target_compile_definitions(thermal_xu PRIVATE HAVE_UVC_LIBS=1)
else()
    # For architectures without UVC libs, just link basic Android libraries
    target_link_libraries(thermal_xu
        log
        android
    )
    target_compile_definitions(thermal_xu PRIVATE HAVE_UVC_LIBS=0)
endif()

# Set compiler flags (no STL, no exceptions, no RTTI)
target_compile_options(thermal_xu PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
)