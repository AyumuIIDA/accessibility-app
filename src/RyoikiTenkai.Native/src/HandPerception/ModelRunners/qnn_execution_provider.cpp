#include "HandPerception/ModelRunners/qnn_execution_provider.h"

#include <sstream>
#include <vector>

#if defined(RYOIKI_ENABLE_WINDOWS_ML_EP_CATALOG)
#include <windows.h>
#include <MddBootstrap.h>
#include <WinMLEpCatalog.h>

#include <filesystem>
#include <mutex>
#endif

namespace ryoiki::hand_perception
{
namespace
{
std::string describeOrtStatus(const Ort::Exception& exception)
{
    std::ostringstream stream;
    stream << exception.what() << " (ORT code " << exception.GetOrtErrorCode() << ")";
    return stream.str();
}

#if defined(RYOIKI_ENABLE_WINDOWS_ML_EP_CATALOG)
std::string hresultToString(const HRESULT value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<unsigned long>(value);
    return stream.str();
}

bool ensureWindowsAppRuntime(std::string& error)
{
    static std::once_flag once;
    static HRESULT result = E_FAIL;
    std::call_once(once, []
    {
        PACKAGE_VERSION minVersion{};
        result = MddBootstrapInitialize2(
            0x00020000,
            nullptr,
            minVersion,
            MddBootstrapInitializeOptions_OnPackageIdentity_NOOP);
    });

    if (FAILED(result))
    {
        error = "Windows App Runtime bootstrap failed: " + hresultToString(result);
        return false;
    }

    return true;
}

struct CatalogHandle
{
    WinMLEpCatalogHandle value{};

    ~CatalogHandle()
    {
        WinMLEpCatalogRelease(value);
    }
};

bool getProviderLibraryPath(WinMLEpHandle provider, std::string& path, std::string& error)
{
    size_t pathSize = 0;
    HRESULT hr = WinMLEpGetLibraryPathSize(provider, &pathSize);
    if (FAILED(hr) || pathSize == 0)
    {
        error = "QNN EP library path size unavailable: " + hresultToString(hr);
        return false;
    }

    path.assign(pathSize, '\0');
    hr = WinMLEpGetLibraryPath(provider, path.size(), path.data(), nullptr);
    if (FAILED(hr))
    {
        error = "QNN EP library path unavailable: " + hresultToString(hr);
        return false;
    }

    while (!path.empty() && path.back() == '\0')
    {
        path.pop_back();
    }

    return !path.empty();
}

bool findQnnProvider(
    const CatalogHandle& catalog,
    WinMLEpHandle& provider,
    std::string& error)
{
    HRESULT hr = WinMLEpCatalogFindProvider(catalog.value, "QNN", nullptr, &provider);
    if (FAILED(hr) || provider == nullptr)
    {
        hr = WinMLEpCatalogFindProvider(catalog.value, "QNNExecutionProvider", nullptr, &provider);
    }
    if (FAILED(hr) || provider == nullptr)
    {
        error = "Windows ML QNN provider was not found: " + hresultToString(hr);
        return false;
    }

    return true;
}

bool appendRegisteredQnnNpuDevice(
    Ort::Env& environment,
    Ort::SessionOptions& sessionOptions,
    std::string& error)
{
    std::vector<Ort::ConstEpDevice> selectedDevices;
    std::ostringstream availableDevices;
    for (const auto& device : environment.GetEpDevices())
    {
        const auto epName = std::string{device.EpName()};
        const auto hardwareType = device.Device().Type();
        if (availableDevices.tellp() > 0)
        {
            availableDevices << "; ";
        }
        availableDevices << epName
            << " type=" << static_cast<int>(hardwareType)
            << " vendor=" << device.Device().Vendor();
        if ((epName == "QNNExecutionProvider" || epName == "QNN")
            && hardwareType == OrtHardwareDeviceType_NPU)
        {
            selectedDevices.push_back(device);
            break;
        }
    }

    if (selectedDevices.empty())
    {
        error = "Windows ML registered QNN but no QNN NPU EP device was enumerated. Devices: "
            + availableDevices.str();
        return false;
    }

    sessionOptions.AppendExecutionProvider_V2(environment, selectedDevices, {});
    return true;
}
#endif
}

bool tryAppendWindowsMlQnnHtp(
    Ort::Env& environment,
    Ort::SessionOptions& sessionOptions,
    std::string& providerDetail,
    std::string& error)
{
#if defined(RYOIKI_ENABLE_WINDOWS_ML_EP_CATALOG)
    try
    {
        if (!ensureWindowsAppRuntime(error))
        {
            return false;
        }

        CatalogHandle catalog;
        HRESULT hr = WinMLEpCatalogCreate(&catalog.value);
        if (FAILED(hr) || catalog.value == nullptr)
        {
            error = "Windows ML EP catalog creation failed: " + hresultToString(hr);
            return false;
        }

        WinMLEpHandle provider = nullptr;
        if (!findQnnProvider(catalog, provider, error))
        {
            return false;
        }

        hr = WinMLEpEnsureReady(provider);
        if (FAILED(hr))
        {
            error = "Windows ML QNN provider is not ready: " + hresultToString(hr);
            return false;
        }

        std::string libraryPath;
        if (!getProviderLibraryPath(provider, libraryPath, error))
        {
            return false;
        }

        try
        {
            environment.RegisterExecutionProviderLibrary(
                "QNN",
                std::filesystem::path{libraryPath}.wstring());
        }
        catch (const Ort::Exception& exception)
        {
            const std::string message = exception.what();
            if (message.find("already registered") == std::string::npos)
            {
                throw;
            }
        }
        if (!appendRegisteredQnnNpuDevice(environment, sessionOptions, error))
        {
            return false;
        }

        providerDetail = "QNNExecutionProvider(WindowsML HTP)";
        return true;
    }
    catch (const Ort::Exception& exception)
    {
        error = "Windows ML QNN registration failed: " + describeOrtStatus(exception);
        return false;
    }
#else
    (void)environment;
    (void)sessionOptions;
    (void)providerDetail;
    error = "Windows ML EP catalog support was not compiled into the native runtime.";
    return false;
#endif
}
}
