cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file")
endif()

project(RyoikiTenkaiNative LANGUAGES CXX)

find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc)

set(RYOIKI_ORT_BACKEND "qnn" CACHE STRING "ONNX Runtime distribution: qnn or directml")
set_property(CACHE RYOIKI_ORT_BACKEND PROPERTY STRINGS qnn directml)
if(RYOIKI_ORT_BACKEND STREQUAL "directml")
    set(_default_onnxruntime_package "microsoft.ml.onnxruntime.directml")
elseif(RYOIKI_ORT_BACKEND STREQUAL "qnn")
    set(_default_onnxruntime_package "microsoft.ml.onnxruntime.qnn")
else()
    message(FATAL_ERROR "RYOIKI_ORT_BACKEND must be 'qnn' or 'directml'.")
endif()
set(ONNXRUNTIME_PACKAGE_NAME "${_default_onnxruntime_package}" CACHE STRING
    "ONNX Runtime NuGet package name" FORCE)
set(ONNXRUNTIME_VERSION "1.24.4" CACHE STRING "ONNX Runtime NuGet package version")
if(NOT DEFINED ONNXRUNTIME_ROOT)
    if(DEFINED ENV{NUGET_PACKAGES})
        set(_nuget_packages "$ENV{NUGET_PACKAGES}")
    elseif(DEFINED ENV{USERPROFILE})
        set(_nuget_packages "$ENV{USERPROFILE}/.nuget/packages")
    endif()
    if(DEFINED _nuget_packages)
        set(ONNXRUNTIME_ROOT
            "${_nuget_packages}/${ONNXRUNTIME_PACKAGE_NAME}/${ONNXRUNTIME_VERSION}"
            CACHE PATH "ONNX Runtime NuGet package root")
    endif()
endif()
cmake_path(CONVERT "${ONNXRUNTIME_ROOT}" TO_CMAKE_PATH_LIST ONNXRUNTIME_ROOT NORMALIZE)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
    set(_onnxruntime_rid "win-arm64")
else()
    set(_onnxruntime_rid "win-x64")
endif()

set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/build/native/include")
set(ONNXRUNTIME_LIBRARY
    "${ONNXRUNTIME_ROOT}/runtimes/${_onnxruntime_rid}/native/onnxruntime.lib")
if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    message(FATAL_ERROR
        "ONNX Runtime headers were not found below ONNXRUNTIME_ROOT='${ONNXRUNTIME_ROOT}'.")
endif()
if(NOT EXISTS "${ONNXRUNTIME_LIBRARY}")
    message(FATAL_ERROR
        "ONNX Runtime import library was not found for ${_onnxruntime_rid} below "
        "ONNXRUNTIME_ROOT='${ONNXRUNTIME_ROOT}'.")
endif()

add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_IMPLIB "${ONNXRUNTIME_LIBRARY}"
    IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/runtimes/${_onnxruntime_rid}/native/onnxruntime.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
)

file(GLOB _onnxruntime_runtime_files CONFIGURE_DEPENDS
    "${ONNXRUNTIME_ROOT}/runtimes/${_onnxruntime_rid}/native/*.dll"
    "${ONNXRUNTIME_ROOT}/runtimes/${_onnxruntime_rid}/native/*.so"
    "${ONNXRUNTIME_ROOT}/runtimes/${_onnxruntime_rid}/native/*.cat")

function(copy_onnxruntime_runtime target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_onnxruntime_runtime_files}
            "$<TARGET_FILE_DIR:${target_name}>"
        COMMAND_EXPAND_LISTS)
endfunction()

add_library(RyoikiTenkai.VisionCore STATIC
    src/Buffers/frame_buffer.cpp
    src/Buffers/frame_pool.cpp
    src/Buffers/tensor_buffer.cpp
    src/Geometry/geometry_types.cpp
    src/Geometry/d3d11_hand_geometry_processor.cpp
    src/Geometry/hand_geometry_processor.cpp
    src/Features/Cad/cad_hand_binding.cpp
    src/HandPerception/MediaPipeGraph/hand_landmark_graph.cpp
    src/HandPerception/MediaPipeGraph/hand_landmarks_to_roi.cpp
    src/HandPerception/MediaPipeGraph/hand_perception_graph.cpp
    src/HandPerception/MediaPipeGraph/palm_detection_graph.cpp
    src/HandPerception/MediaPipeGraph/palm_detection_postprocessor.cpp
    src/HandPerception/MediaPipeGraph/palm_detection_to_roi.cpp
    src/HandInput/Measurements/gesture_candidate_window_history.cpp
    src/HandInput/Measurements/hand_measurement_extractor.cpp
    src/HandInput/Measurements/hand_topology_history.cpp
    src/HandInput/Measurements/hand_unified_feature_frame.cpp
    src/HandInput/Measurements/multi_hand_measurement_stage.cpp
    src/HandInput/Measurements/palm_basis_rotation_tracker.cpp
    src/HandInput/Measurements/palm_rotation_eskf.cpp
    src/HandInput/Measurements/palm_rotation_one_euro_filter.cpp
    src/HandInput/Measurements/rotation_observation_gate.cpp
    src/HandInput/Measurements/weighted_palm_rotation_tracker.cpp
    src/HandInput/Publication/latest_hand_state_slot.cpp
    src/HandInput/Publication/ordered_hand_event_ring.cpp
    src/HandInput/Recognition/domain_expansion_state_recognizer.cpp
    src/HandInput/Recognition/gesture_recording_session.cpp
    src/HandInput/Recognition/gesture_template_repository.cpp
    src/HandInput/Recognition/gesture_template_serialization.cpp
    src/HandInput/Recognition/gesture_match_stabilizer.cpp
    src/HandInput/Recognition/multi_hand_gesture_core.cpp
    src/HandInput/Recognition/gesture_template_registry.cpp
    src/HandInput/Recognition/open_palm_state_recognizer.cpp
    src/HandInput/Recognition/swipe_event_recognizer.cpp
    src/HandInput/Recognition/timed_state_stabilizer.cpp
    src/HandInput/Recognition/unified_sequence_gesture_comparator.cpp
    src/HandPerception/ModelRunners/ort_hand_landmark_runner.cpp
    src/HandPerception/ModelRunners/ort_palm_detection_runner.cpp
    src/HandPerception/ModelRunners/ort_session_configuration.cpp
    src/HandPerception/ModelRunners/calibration_tensor_capture.cpp
    src/HandPerception/ModelRunners/hand_landmark_runner.h
    src/HandPerception/ModelRunners/hand_landmark_model_contract.cpp
    src/HandPerception/ModelRunners/palm_detection_runner.cpp
    src/Pipeline/latest_frame_slot.cpp
    src/Pipeline/perception_mailbox.cpp
    src/Presentation/hand_presentation_transform.cpp
    src/Rendering/frame_transforms.cpp
    src/Rendering/gesture_dtw_debug_layout.cpp
    src/Rendering/gesture_dtw_debug_render_packet.cpp
    src/Rendering/gesture_dtw_debug_source.cpp
    src/Rendering/gesture_dtw_debug_render_packet_adapter.cpp
    src/Rendering/hand_3d_plot.cpp
    src/Rendering/latest_render_packet_slot.cpp
    src/Runtime/camera_device_selection.cpp
    src/Runtime/d3d11_device.cpp
    src/Runtime/frame_orientation.cpp
)

target_include_directories(RyoikiTenkai.VisionCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(RyoikiTenkai.VisionCore PUBLIC cxx_std_20)

if(RYOIKI_ORT_BACKEND STREQUAL "directml")
    target_sources(RyoikiTenkai.VisionCore PRIVATE
        src/Runtime/directml_runtime.cpp
        src/Geometry/d3d12_hand_geometry_processor.cpp)
    target_compile_definitions(RyoikiTenkai.VisionCore PUBLIC RYOIKI_ORT_DIRECTML=1)
    set(DIRECTML_PACKAGE_ROOT "$ENV{USERPROFILE}/.nuget/packages/microsoft.ai.directml/1.15.4")
    target_include_directories(RyoikiTenkai.VisionCore PUBLIC
        "${DIRECTML_PACKAGE_ROOT}/include")
    target_link_libraries(RyoikiTenkai.VisionCore PUBLIC
        "${DIRECTML_PACKAGE_ROOT}/bin/arm64-win/DirectML.lib"
        d3d12 dxgi)
else()
    target_compile_definitions(RyoikiTenkai.VisionCore PUBLIC RYOIKI_ORT_QNN=1)
endif()

target_link_libraries(RyoikiTenkai.VisionCore PUBLIC
    opencv_core
    opencv_imgproc
    onnxruntime
    d3d11
    d3dcompiler
    d3d12
    winsqlite3
)

add_library(RyoikiTenkai.Native SHARED
    src/cad_runtime.cpp
    src/camera_capture.cpp
    src/Rendering/cad_renderer.cpp
    src/Rendering/cad_render_stage.cpp
    src/Rendering/d3d11_d2d_renderer.cpp
    src/Rendering/gesture_dtw_debug_renderer.cpp
    src/Rendering/gesture_dtw_debug_render_stage.cpp
    src/Rendering/native_render_stage.cpp
    src/gesture_dtw_debug_runtime.cpp
    src/gesture_recording_playback_runtime.cpp
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
    d2d1
    dwrite
    d3d11
    dxgi
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
copy_onnxruntime_runtime(RyoikiTenkai.Native)

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
    copy_onnxruntime_runtime(RyoikiTenkai.VisionCore.Tests)

    add_executable(RyoikiTenkai.HandPerception.Tests
        tests/hand_perception_tests.cpp
    )
    target_link_libraries(RyoikiTenkai.HandPerception.Tests PRIVATE
        RyoikiTenkai.VisionCore
    )
    target_include_directories(RyoikiTenkai.HandPerception.Tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(RyoikiTenkai.HandPerception.Tests PRIVATE
        RYOIKI_PALM_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../RyoikiTenkai/models/palm_detection.onnx"
        RYOIKI_HAND_MODEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../RyoikiTenkai/models/hand_landmark.onnx"
    )
    if (MSVC)
        target_compile_options(RyoikiTenkai.HandPerception.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.HandPerception.Tests COMMAND RyoikiTenkai.HandPerception.Tests)
    copy_onnxruntime_runtime(RyoikiTenkai.HandPerception.Tests)

    add_executable(RyoikiTenkai.HandUnifiedFeatureFrame.Tests
        tests/hand_unified_feature_frame_tests.cpp
    )
    target_link_libraries(RyoikiTenkai.HandUnifiedFeatureFrame.Tests PRIVATE
        RyoikiTenkai.VisionCore
    )
    if (MSVC)
        target_compile_options(RyoikiTenkai.HandUnifiedFeatureFrame.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.HandUnifiedFeatureFrame.Tests COMMAND RyoikiTenkai.HandUnifiedFeatureFrame.Tests)
    copy_onnxruntime_runtime(RyoikiTenkai.HandUnifiedFeatureFrame.Tests)

    add_executable(RyoikiTenkai.GestureCandidateWindowHistory.Tests
        tests/gesture_candidate_window_history_tests.cpp
    )
    target_link_libraries(RyoikiTenkai.GestureCandidateWindowHistory.Tests PRIVATE
        RyoikiTenkai.VisionCore
    )
    if (MSVC)
        target_compile_options(RyoikiTenkai.GestureCandidateWindowHistory.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.GestureCandidateWindowHistory.Tests COMMAND RyoikiTenkai.GestureCandidateWindowHistory.Tests)
    copy_onnxruntime_runtime(RyoikiTenkai.GestureCandidateWindowHistory.Tests)

    add_executable(RyoikiTenkai.UnifiedSequenceGestureComparator.Tests
        tests/unified_sequence_gesture_comparator_tests.cpp
    )
    target_link_libraries(RyoikiTenkai.UnifiedSequenceGestureComparator.Tests PRIVATE
        RyoikiTenkai.VisionCore
    )
    if (MSVC)
        target_compile_options(RyoikiTenkai.UnifiedSequenceGestureComparator.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.UnifiedSequenceGestureComparator.Tests COMMAND RyoikiTenkai.UnifiedSequenceGestureComparator.Tests)
    copy_onnxruntime_runtime(RyoikiTenkai.UnifiedSequenceGestureComparator.Tests)

    add_executable(RyoikiTenkai.GestureTemplateRegistry.Tests
        tests/gesture_template_registry_tests.cpp
    )
    target_link_libraries(RyoikiTenkai.GestureTemplateRegistry.Tests PRIVATE
        RyoikiTenkai.VisionCore
    )
    if (MSVC)
        target_compile_options(RyoikiTenkai.GestureTemplateRegistry.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.GestureTemplateRegistry.Tests COMMAND RyoikiTenkai.GestureTemplateRegistry.Tests)
    copy_onnxruntime_runtime(RyoikiTenkai.GestureTemplateRegistry.Tests)

    add_executable(RyoikiTenkai.GestureTemplateRepository.Tests tests/gesture_template_repository_tests.cpp)
    target_link_libraries(RyoikiTenkai.GestureTemplateRepository.Tests PRIVATE RyoikiTenkai.VisionCore)
    if (MSVC)
        target_compile_options(RyoikiTenkai.GestureTemplateRepository.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.GestureTemplateRepository.Tests COMMAND RyoikiTenkai.GestureTemplateRepository.Tests)

    add_executable(RyoikiTenkai.GestureDtwVisualizationPacket.Tests
        tests/gesture_dtw_visualization_packet_tests.cpp
    )
    target_link_libraries(RyoikiTenkai.GestureDtwVisualizationPacket.Tests PRIVATE
        RyoikiTenkai.VisionCore
    )
    if (MSVC)
        target_compile_options(RyoikiTenkai.GestureDtwVisualizationPacket.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.GestureDtwVisualizationPacket.Tests COMMAND RyoikiTenkai.GestureDtwVisualizationPacket.Tests)
    copy_onnxruntime_runtime(RyoikiTenkai.GestureDtwVisualizationPacket.Tests)

    add_executable(RyoikiTenkai.GestureRecordingSession.Tests
        tests/gesture_recording_session_tests.cpp
    )
    target_link_libraries(RyoikiTenkai.GestureRecordingSession.Tests PRIVATE
        RyoikiTenkai.VisionCore
    )
    if (MSVC)
        target_compile_options(RyoikiTenkai.GestureRecordingSession.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.GestureRecordingSession.Tests COMMAND RyoikiTenkai.GestureRecordingSession.Tests)
    copy_onnxruntime_runtime(RyoikiTenkai.GestureRecordingSession.Tests)

    add_executable(RyoikiTenkai.GestureMatchStabilizer.Tests
        tests/gesture_match_stabilizer_tests.cpp)
    target_link_libraries(RyoikiTenkai.GestureMatchStabilizer.Tests PRIVATE RyoikiTenkai.VisionCore)
    if (MSVC)
        target_compile_options(RyoikiTenkai.GestureMatchStabilizer.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.GestureMatchStabilizer.Tests COMMAND RyoikiTenkai.GestureMatchStabilizer.Tests)
    copy_onnxruntime_runtime(RyoikiTenkai.GestureMatchStabilizer.Tests)

    add_executable(RyoikiTenkai.MultiHandGestureCore.Tests
        tests/multi_hand_gesture_core_tests.cpp)
    target_link_libraries(RyoikiTenkai.MultiHandGestureCore.Tests PRIVATE RyoikiTenkai.VisionCore)
    if (MSVC)
        target_compile_options(RyoikiTenkai.MultiHandGestureCore.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.MultiHandGestureCore.Tests COMMAND RyoikiTenkai.MultiHandGestureCore.Tests)
    copy_onnxruntime_runtime(RyoikiTenkai.MultiHandGestureCore.Tests)

    add_executable(RyoikiTenkai.NativeAbi.Tests
        tests/ryoiki_native_abi_tests.cpp
    )
    target_include_directories(RyoikiTenkai.NativeAbi.Tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_features(RyoikiTenkai.NativeAbi.Tests PRIVATE cxx_std_20)
    if (MSVC)
        target_compile_options(RyoikiTenkai.NativeAbi.Tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME RyoikiTenkai.NativeAbi.Tests COMMAND RyoikiTenkai.NativeAbi.Tests)
endif()
