# Copyright (C) Microsoft Corporation. All rights reserved.
#
# CMake targets file for Microsoft.Windows.AI.MachineLearning
# This file defines the imported targets for the package.
#
# This file must be included via the config file (microsoft.windows.ai.machinelearning-config.cmake)
# which sets _WINML_INCLUDE_DIR, _WINML_LIB_DIR, and _WINML_BIN_DIR.

if(NOT DEFINED _WINML_LIB_DIR)
    message(FATAL_ERROR
        "microsoft.windows.ai.machinelearning-targets.cmake must be included via "
        "microsoft.windows.ai.machinelearning-config.cmake, not directly.")
endif()

# DirectML runtime DLL (loaded dynamically by OnnxRuntime when using the DML
# execution provider). DirectML has no import library, so this is an INTERFACE
# target — linking it does not produce linker input. Add ${WINML_DIRECTML_DLL}
# to your post-build copy command if you need to deploy it.
add_library(WindowsML::DirectML INTERFACE IMPORTED)

# OnnxRuntime target for direct C API access
add_library(WindowsML::OnnxRuntime SHARED IMPORTED)
set_target_properties(WindowsML::OnnxRuntime PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_WINML_INCLUDE_DIR}/winml"
    IMPORTED_IMPLIB "${_WINML_LIB_DIR}/onnxruntime.lib"
    IMPORTED_LOCATION "${_WINML_BIN_DIR}/onnxruntime.dll"
)

# Windows ML API target
add_library(WindowsML::Api SHARED IMPORTED)
set_target_properties(WindowsML::Api PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_WINML_INCLUDE_DIR}"
    IMPORTED_IMPLIB "${_WINML_LIB_DIR}/Microsoft.Windows.AI.MachineLearning.lib"
    IMPORTED_LOCATION "${_WINML_BIN_DIR}/Microsoft.Windows.AI.MachineLearning.dll"
)

# Meta-target that links all components (Api + OnnxRuntime + DirectML)
add_library(WindowsML::WindowsML INTERFACE IMPORTED)
set_target_properties(WindowsML::WindowsML PROPERTIES
    INTERFACE_LINK_LIBRARIES "WindowsML::Api;WindowsML::OnnxRuntime;WindowsML::DirectML"
)
