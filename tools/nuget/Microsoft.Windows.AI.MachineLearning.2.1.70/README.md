# Using Microsoft.Windows.AI.MachineLearning

The `Microsoft.Windows.AI.MachineLearning` package provides Windows Machine Learning APIs for
on-device AI inference using ONNX Runtime.

## Requirements

- **Minimum OS:** Windows 10 19H1 (build 18362) or later.
- For Windows 10 versions prior to 19H1 (build 18362), use the `Microsoft.WindowsAppSDK.ML` package
  instead, which provides framework-based deployment via the Windows App SDK runtime.

## Getting Started

Add a reference to `Microsoft.Windows.AI.MachineLearning` in your project. No additional
configuration is required for unpackaged apps on Windows 10 19H1+ and Windows 11.

### Native C++

```cpp
#include <winrt/Microsoft.Windows.AI.MachineLearning.h>
```

### .NET

```csharp
using Microsoft.Windows.AI.MachineLearning;
```

## Deployment

Binaries are deployed alongside your application:

```
MyApp/
├── MyApp.exe
├── Microsoft.Windows.AI.MachineLearning.dll
├── onnxruntime.dll
└── DirectML.dll
```

WinRT activation relies on OS-native reg-free WinRT, available on Windows 10 19H1+.

## Using ONNX Runtime Headers

The ONNX Runtime headers are included in a `winml` subdirectory:

```cpp
#include <winml/onnxruntime_c_api.h>
#include <winml/onnxruntime_cxx_api.h>
```

To use headers without the `winml/` prefix:

```xml
<PropertyGroup>
  <WinMLEnableDefaultOrtHeaderIncludePath>true</WinMLEnableDefaultOrtHeaderIncludePath>
</PropertyGroup>
```

## WindowsAppSDK Integration

For WindowsAppSDK consumers or packaged (MSIX) applications, use
`Microsoft.WindowsAppSDK.ML` instead. It depends on this package transitively and provides
additional integration with the Windows App SDK runtime.

## More Information

- [Windows ML documentation](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/overview)
- [Windows ML Samples](https://github.com/microsoft/WindowsML)
- [Windows App SDK](https://github.com/microsoft/WindowsAppSDK)
- [WindowsAppSDK-Samples](https://github.com/microsoft/WindowsAppSDK-Samples)
