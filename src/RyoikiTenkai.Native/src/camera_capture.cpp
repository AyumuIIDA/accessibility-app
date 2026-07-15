#include "camera_capture.h"
#include "Runtime/camera_device_selection.h"

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

std::string formatHresult(const char* operation, const HRESULT result)
{
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex
            << static_cast<unsigned long>(result) << '.';
    return message.str();
}

bool failed(const HRESULT result, const char* operation, std::string& error)
{
    if (SUCCEEDED(result))
    {
        return false;
    }

    error = formatHresult(operation, result);
    return true;
}

std::wstring getCameraFriendlyName(IMFActivate& device)
{
    wchar_t* allocatedName = nullptr;
    UINT32 nameLength = 0;
    if (FAILED(device.GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
            &allocatedName,
            &nameLength)))
    {
        return L"Unnamed camera";
    }

    std::wstring name{allocatedName, nameLength};
    CoTaskMemFree(allocatedName);
    return name;
}
}

struct CameraCapture::Impl
{
    bool comInitialized{false};
    bool mediaFoundationStarted{false};
    ComPtr<IMFMediaSource> mediaSource;
    ComPtr<IMFSourceReader> sourceReader;
    std::uint32_t width{0};
    std::uint32_t height{0};

    ~Impl()
    {
        sourceReader.Reset();
        if (mediaSource != nullptr)
        {
            mediaSource->Shutdown();
            mediaSource.Reset();
        }

        if (mediaFoundationStarted)
        {
            MFShutdown();
        }

        if (comInitialized)
        {
            CoUninitialize();
        }
    }
};

CameraCapture::CameraCapture() : impl_{std::make_unique<Impl>()}
{
}

CameraCapture::~CameraCapture() = default;

bool CameraCapture::initialize(std::string& error)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (failed(comResult, "CoInitializeEx", error))
    {
        return false;
    }
    impl_->comInitialized = true;

    const HRESULT startupResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (failed(startupResult, "MFStartup", error))
    {
        return false;
    }
    impl_->mediaFoundationStarted = true;

    ComPtr<IMFAttributes> deviceAttributes;
    HRESULT result = MFCreateAttributes(&deviceAttributes, 1);
    if (failed(result, "MFCreateAttributes", error))
    {
        return false;
    }

    result = deviceAttributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (failed(result, "Set camera source type", error))
    {
        return false;
    }

    IMFActivate** devices = nullptr;
    UINT32 deviceCount = 0;
    result = MFEnumDeviceSources(deviceAttributes.Get(), &devices, &deviceCount);
    if (failed(result, "MFEnumDeviceSources", error))
    {
        return false;
    }

    if (deviceCount == 0)
    {
        error = "No video capture device was found.";
        CoTaskMemFree(devices);
        return false;
    }

    std::vector<std::wstring> friendlyNames;
    friendlyNames.reserve(deviceCount);
    for (UINT32 index = 0; index < deviceCount; ++index)
    {
        friendlyNames.push_back(getCameraFriendlyName(*devices[index]));
        const std::wstring diagnostic = L"Native camera candidate "
            + std::to_wstring(index) + L": " + friendlyNames.back() + L"\n";
        OutputDebugStringW(diagnostic.c_str());
    }

    const std::size_t selectedIndex = ryoiki::runtime::selectUserFacingCamera(friendlyNames);
    const std::wstring selectedDiagnostic = L"Native camera selected: "
        + friendlyNames[selectedIndex] + L"\n";
    OutputDebugStringW(selectedDiagnostic.c_str());
    result = devices[selectedIndex]->ActivateObject(IID_PPV_ARGS(&impl_->mediaSource));
    for (UINT32 index = 0; index < deviceCount; ++index)
    {
        devices[index]->Release();
    }
    CoTaskMemFree(devices);

    if (failed(result, "Activate camera", error))
    {
        return false;
    }

    ComPtr<IMFAttributes> readerAttributes;
    result = MFCreateAttributes(&readerAttributes, 1);
    if (failed(result, "Create source reader attributes", error))
    {
        return false;
    }

    result = readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (failed(result, "Enable source reader video processing", error))
    {
        return false;
    }

    result = MFCreateSourceReaderFromMediaSource(
        impl_->mediaSource.Get(),
        readerAttributes.Get(),
        &impl_->sourceReader);
    if (failed(result, "MFCreateSourceReaderFromMediaSource", error))
    {
        return false;
    }

    ComPtr<IMFMediaType> outputType;
    result = MFCreateMediaType(&outputType);
    if (failed(result, "MFCreateMediaType", error))
    {
        return false;
    }

    if (failed(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set video major type", error)
        || failed(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "Set RGB32 subtype", error))
    {
        return false;
    }

    result = impl_->sourceReader->SetCurrentMediaType(
        kVideoStream,
        nullptr,
        outputType.Get());
    if (failed(result, "Set source reader RGB32 output", error))
    {
        return false;
    }

    ComPtr<IMFMediaType> currentType;
    result = impl_->sourceReader->GetCurrentMediaType(
        kVideoStream,
        &currentType);
    if (failed(result, "Get source reader media type", error))
    {
        return false;
    }

    UINT32 width = 0;
    UINT32 height = 0;
    result = MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (failed(result, "Get camera frame size", error) || width == 0 || height == 0)
    {
        if (error.empty())
        {
            error = "The camera returned an invalid frame size.";
        }
        return false;
    }

    impl_->width = width;
    impl_->height = height;
    return true;
}

void CameraCapture::requestStop() noexcept
{
    if (impl_->sourceReader != nullptr)
    {
        impl_->sourceReader->Flush(kVideoStream);
    }
    if (impl_->mediaSource != nullptr)
    {
        impl_->mediaSource->Shutdown();
    }
}

bool CameraCapture::readFrame(
    std::vector<std::uint8_t>& bgra,
    std::uint32_t& width,
    std::uint32_t& height,
    double& cameraWaitMs,
    double& frameCopyMs,
    std::string& error)
{
    using clock = std::chrono::steady_clock;

    if (impl_->sourceReader == nullptr)
    {
        error = "Camera capture is not initialized.";
        return false;
    }

    ComPtr<IMFSample> sample;
    DWORD streamIndex = 0;
    DWORD streamFlags = 0;
    LONGLONG sampleTime = 0;
    const auto waitStarted = clock::now();

    while (sample == nullptr)
    {
        const HRESULT result = impl_->sourceReader->ReadSample(
            kVideoStream,
            0,
            &streamIndex,
            &streamFlags,
            &sampleTime,
            &sample);
        if (failed(result, "Read camera sample", error))
        {
            return false;
        }

        if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
        {
            error = "The camera stream ended.";
            return false;
        }

        if ((streamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0)
        {
            error = "The camera changed media type while running.";
            return false;
        }
    }

    const auto waitCompleted = clock::now();
    cameraWaitMs = std::chrono::duration<double, std::milli>(waitCompleted - waitStarted).count();

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT result = sample->ConvertToContiguousBuffer(&buffer);
    if (failed(result, "Convert camera sample buffer", error))
    {
        return false;
    }

    const auto requiredBytes64 = static_cast<std::uint64_t>(impl_->width)
        * static_cast<std::uint64_t>(impl_->height) * 4U;
    if (requiredBytes64 > std::numeric_limits<std::size_t>::max())
    {
        error = "The camera frame is too large for this process.";
        return false;
    }

    const auto copyStarted = clock::now();
    bgra.resize(static_cast<std::size_t>(requiredBytes64));

    ComPtr<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer.As(&buffer2d)))
    {
        BYTE* firstScanline = nullptr;
        LONG pitch = 0;
        result = buffer2d->Lock2D(&firstScanline, &pitch);
        if (failed(result, "Lock camera 2D buffer", error))
        {
            return false;
        }

        const auto rowBytes = static_cast<std::size_t>(impl_->width) * 4U;
        for (std::uint32_t y = 0; y < impl_->height; ++y)
        {
            const auto* sourceRow = firstScanline + static_cast<std::ptrdiff_t>(y) * pitch;
            auto* destinationRow = bgra.data() + static_cast<std::size_t>(y) * rowBytes;
            std::memcpy(destinationRow, sourceRow, rowBytes);
        }
        buffer2d->Unlock2D();
    }
    else
    {
        BYTE* data = nullptr;
        DWORD maximumLength = 0;
        DWORD currentLength = 0;
        result = buffer->Lock(&data, &maximumLength, &currentLength);
        if (failed(result, "Lock camera buffer", error))
        {
            return false;
        }

        if (currentLength < requiredBytes64)
        {
            buffer->Unlock();
            error = "The camera buffer is smaller than the declared frame size.";
            return false;
        }

        const auto rowBytes = static_cast<std::size_t>(impl_->width) * 4U;
        for (std::uint32_t y = 0; y < impl_->height; ++y)
        {
            const auto sourceY = impl_->height - 1U - y;
            const auto* sourceRow = data + static_cast<std::size_t>(sourceY) * rowBytes;
            auto* destinationRow = bgra.data() + static_cast<std::size_t>(y) * rowBytes;
            std::memcpy(destinationRow, sourceRow, rowBytes);
        }
        buffer->Unlock();
    }

    const auto copyCompleted = clock::now();
    frameCopyMs = std::chrono::duration<double, std::milli>(copyCompleted - copyStarted).count();
    width = impl_->width;
    height = impl_->height;
    return true;
}
