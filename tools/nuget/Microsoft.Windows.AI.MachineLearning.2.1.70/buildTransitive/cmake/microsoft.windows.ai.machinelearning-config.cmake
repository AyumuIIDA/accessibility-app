# Copyright (C) Microsoft Corporation. All rights reserved.
#
# CMake config file for Microsoft.Windows.AI.MachineLearning
#
# This module defines the following IMPORTED targets:
#   WindowsML::WindowsML   - Meta-target linking all components below
#   WindowsML::Api         - Windows ML API (Microsoft.Windows.AI.MachineLearning)
#   WindowsML::OnnxRuntime - OnnxRuntime C API
#   WindowsML::DirectML    - DirectML (INTERFACE only — no import library)
#
# Targets are independent — link only what you need:
#   target_link_libraries(MyApp PRIVATE WindowsML::WindowsML)                      # Everything
#   target_link_libraries(MyApp PRIVATE WindowsML::Api)                            # WinML API only
#   target_link_libraries(MyApp PRIVATE WindowsML::OnnxRuntime)                    # ORT only (CPU and EP Registration)
#   target_link_libraries(MyApp PRIVATE WindowsML::Api WindowsML::OnnxRuntime)     # Both APIs
#
# Copy runtime DLLs to the build output directory (CMake 3.21+):
#   add_custom_command(TARGET MyApp POST_BUILD
#       COMMAND ${CMAKE_COMMAND} -E copy_if_different
#           $<TARGET_RUNTIME_DLLS:MyApp> $<TARGET_FILE_DIR:MyApp>
#       COMMAND_EXPAND_LISTS
#   )
#
# DirectML has no import library, so it is not included in $<TARGET_RUNTIME_DLLS>.
# If you use the DirectML execution provider, also deploy DirectML.dll:
#   add_custom_command(TARGET MyApp POST_BUILD
#       COMMAND ${CMAKE_COMMAND} -E copy_if_different
#           "${WINML_DIRECTML_DLL}" $<TARGET_FILE_DIR:MyApp>
#   )

cmake_minimum_required(VERSION 3.21)

# Prevent re-inclusion
if(TARGET WindowsML::WindowsML)
    return()
endif()

# Compute the NuGet package root (two levels up from build/cmake/).
cmake_path(SET _WINML_CONFIG_FILE_DIR "${CMAKE_CURRENT_LIST_DIR}")
cmake_path(GET _WINML_CONFIG_FILE_DIR PARENT_PATH _WINML_CONFIG_PARENT_DIR)
cmake_path(GET _WINML_CONFIG_PARENT_DIR PARENT_PATH _WINML_CONFIG_ROOT)

set(WINML_VERSION "2.1.70")

# Determine target architecture from the CMake generator / platform.
if(CMAKE_GENERATOR_PLATFORM)
    string(TOUPPER "${CMAKE_GENERATOR_PLATFORM}" _WINML_PLATFORM_UPPER)
elseif(CMAKE_VS_PLATFORM_NAME)
    string(TOUPPER "${CMAKE_VS_PLATFORM_NAME}" _WINML_PLATFORM_UPPER)
elseif(CMAKE_SYSTEM_PROCESSOR)
    string(TOUPPER "${CMAKE_SYSTEM_PROCESSOR}" _WINML_PLATFORM_UPPER)
else()
    set(_WINML_PLATFORM_UPPER "X64")
endif()

if(_WINML_PLATFORM_UPPER MATCHES "^(AMD64|X64|X86_64)$")
    set(_WINML_ARCH "x64")
    set(_WINML_NUGET_ARCH "win-x64")
elseif(_WINML_PLATFORM_UPPER MATCHES "^(ARM64|AARCH64)$")
    set(_WINML_ARCH "arm64")
    set(_WINML_NUGET_ARCH "win-arm64")
else()
    message(FATAL_ERROR "microsoft.windows.ai.machinelearning: Unsupported architecture '${_WINML_PLATFORM_UPPER}'."
        " Supported architectures: x64, ARM64.")
endif()

# NuGet package layout:
#   include/                          - headers
#   lib/native/<arch>/                - import libraries
#   runtimes/win-<arch>/native/       - runtime DLLs
set(_WINML_INCLUDE_DIR "${_WINML_CONFIG_ROOT}/include")
set(_WINML_LIB_DIR "${_WINML_CONFIG_ROOT}/lib/native/${_WINML_ARCH}")
set(_WINML_BIN_DIR "${_WINML_CONFIG_ROOT}/runtimes/${_WINML_NUGET_ARCH}/native")

# Include the targets file that defines the imported targets
include("${CMAKE_CURRENT_LIST_DIR}/microsoft.windows.ai.machinelearning-targets.cmake")

# Provide helpful variables for consumers
set(WINML_INCLUDE_DIR "${_WINML_INCLUDE_DIR}")
set(WINML_LIBRARY_DIR "${_WINML_LIB_DIR}")
set(WINML_BINARY_DIR "${_WINML_BIN_DIR}")
set(WINML_DIRECTML_DLL "${_WINML_BIN_DIR}/DirectML.dll")

# Clean up internal variables
unset(_WINML_CONFIG_FILE_DIR)
unset(_WINML_CONFIG_PARENT_DIR)
unset(_WINML_CONFIG_ROOT)
unset(_WINML_PLATFORM_UPPER)
unset(_WINML_ARCH)
unset(_WINML_NUGET_ARCH)
