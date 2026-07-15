cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file")
endif()

project(RyoikiTenkaiNative LANGUAGES CXX)

find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc)

set(ONNXRUNTIME_VERSION "1.27.0" CACHE STRING "ONNX Runtime NuGet package version")
if(NOT DEFINED ONNXRUNTIME_ROOT)
    if(DEFINED ENV{NUGET_PACKAGES})
        set(_nuget_packages "$ENV{NUGET_PACKAGES}")
    elseif(DEFINED ENV{USERPROFILE})
        set(_nuget_packages "$ENV{USERPROFILE}/.nuget/packages")
    endif()
    if(DEFINED _nuget_packages)
        set(ONNXRUNTIME_ROOT
            "${_nuget_packages}/microsoft.ml.onnxruntime/${ONNXRUNTIME_VERSION}"
            CACHE PATH "ONNX Runtime NuGet package root")
    endif()
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
    set(_onnxruntime_rid "win-arm64")
else()
    set(_onnxruntime_rid "win-x64")
endif()

find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    PATHS "${ONNXRUNTIME_ROOT}/build/native/include"
    NO_DEFAULT_PATH
    REQUIRED)
find_library(ONNXRUNTIME_LIBRARY onnxruntime
    PATHS "${ONNXRUNTIME_ROOT}/runtimes/${_onnxruntime_rid}/native"
    NO_DEFAULT_PATH
    REQUIRED)

add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_IMPLIB "${ONNXRUNTIME_LIBRARY}"
    IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/runtimes/${_onnxruntime_rid}/native/onnxruntime.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
)

add_library(RyoikiTenkai.VisionCore STATIC
    src/Buffers/frame_buffer.cpp
    src/Buffers/frame_pool.cpp
    src/Buffers/tensor_buffer.cpp
    src/Geometry/geometry_types.cpp
    src/Geometry/hand_geometry_processor.cpp
    src/HandPerception/MediaPipeGraph/hand_landmark_graph.cpp
    src/HandPerception/MediaPipeGraph/hand_landmarks_to_roi.cpp
    src/HandPerception/MediaPipeGraph/hand_perception_graph.cpp
    src/HandPerception/MediaPipeGraph/palm_detection_graph.cpp
    src/HandPerception/MediaPipeGraph/palm_detection_postprocessor.cpp
    src/HandPerception/MediaPipeGraph/palm_detection_to_roi.cpp
    src/HandPerception/ModelRunners/cpu_hand_landmark_runner.cpp
    src/HandPerception/ModelRunners/cpu_palm_detection_runner.cpp
    src/HandPerception/ModelRunners/hand_landmark_runner.h
    src/HandPerception/ModelRunners/hand_landmark_model_contract.cpp
    src/HandPerception/ModelRunners/palm_detection_runner.cpp
    src/Pipeline/latest_frame_slot.cpp
    src/Pipeline/perception_mailbox.cpp
    src/Runtime/camera_device_selection.cpp
)

target_include_directories(RyoikiTenkai.VisionCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(RyoikiTenkai.VisionCore PUBLIC cxx_std_20)

target_link_libraries(RyoikiTenkai.VisionCore PUBLIC
    opencv_core
    opencv_imgproc
    onnxruntime
)

add_library(RyoikiTenkai.Native SHARED
    src/camera_capture.cpp
    src/ryoiki_native.cpp
)

target_include_directories(RyoikiTenkai.Native
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(RyoikiTenkai.Native PRIVATE cxx_std_20)

target_link_libraries(RyoikiTenkai.Native PRIVATE
    RyoikiTenkai.VisionCore
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
)

if (MSVC)
    target_compile_options(RyoikiTenkai.VisionCore PRIVATE /W4 /permissive- /EHsc)
    target_compile_definitions(RyoikiTenkai.VisionCore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(RyoikiTenkai.Native PRIVATE /W4 /permissive- /EHsc)
    target_compile_definitions(RyoikiTenkai.Native PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

set_target_properties(RyoikiTenkai.Native PROPERTIES
    OUTPUT_NAME "RyoikiTenkai.Native"
)

include(CTest)
if (BUILD_TESTING)
    add_executable(RyoikiTenkai.VisionCore.Tests
        tests/vision_core_tests.cpp
    )
    target_link_libraries(RyoikiTenkai.VisionCore.Tests PRIVATE
        RyoikiTenkai.VisionCore
    )
    if (MSVC)
        target_compile_options(RyoikiTenkai.VisionCore.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.VisionCore.Tests COMMAND RyoikiTenkai.VisionCore.Tests)
    add_custom_command(TARGET RyoikiTenkai.VisionCore.Tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:onnxruntime>"
            "$<TARGET_FILE_DIR:RyoikiTenkai.VisionCore.Tests>"
    )

    add_executable(RyoikiTenkai.HandPerception.Tests
        tests/hand_perception_tests.cpp
    )
    target_link_libraries(RyoikiTenkai.HandPerception.Tests PRIVATE
        RyoikiTenkai.VisionCore
    )
    target_compile_definitions(RyoikiTenkai.HandPerception.Tests PRIVATE
        RYOIKI_PALM_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../RyoikiTenkai/models/palm_detection.onnx"
        RYOIKI_HAND_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../RyoikiTenkai/models/hand_landmark.onnx"
    )
    if (MSVC)
        target_compile_options(RyoikiTenkai.HandPerception.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.HandPerception.Tests COMMAND RyoikiTenkai.HandPerception.Tests)
    add_custom_command(TARGET RyoikiTenkai.HandPerception.Tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:onnxruntime>"
            "$<TARGET_FILE_DIR:RyoikiTenkai.HandPerception.Tests>"
    )
endif()
