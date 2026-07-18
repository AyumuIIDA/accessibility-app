#include "HandPerception/ModelRunners/calibration_tensor_capture.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace ryoiki::hand_perception
{
namespace
{
constexpr std::size_t kDefaultCaptureLimit = 512;
constexpr std::size_t kMaximumCaptureLimit = 10000;

std::filesystem::path getCaptureRoot()
{
    const DWORD required = GetEnvironmentVariableW(L"RYOIKI_CALIBRATION_DIR", nullptr, 0);
    if (required <= 1)
    {
        return {};
    }

    std::vector<wchar_t> value(required);
    if (GetEnvironmentVariableW(
            L"RYOIKI_CALIBRATION_DIR", value.data(), required) == 0)
    {
        return {};
    }
    return std::filesystem::path{value.data()};
}

std::size_t getCaptureLimit()
{
    std::array<wchar_t, 32> value{};
    const DWORD length = GetEnvironmentVariableW(
        L"RYOIKI_CALIBRATION_LIMIT", value.data(), static_cast<DWORD>(value.size()));
    if (length == 0 || length >= value.size())
    {
        return kDefaultCaptureLimit;
    }

    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(value.data(), &end, 10);
    if (end == value.data() || *end != L'\0' || parsed == 0)
    {
        return kDefaultCaptureLimit;
    }
    return (std::min)(static_cast<std::size_t>(parsed), kMaximumCaptureLimit);
}
}

CalibrationTensorCapture::CalibrationTensorCapture(
    const std::string_view modelName,
    const std::array<std::int64_t, 4> shape) noexcept
    : shape_{shape}, processId_{GetCurrentProcessId()}
{
    try
    {
        const auto root = getCaptureRoot();
        if (root.empty())
        {
            return;
        }

        outputDirectory_ = root / modelName;
        std::filesystem::create_directories(outputDirectory_);
        limit_ = getCaptureLimit();

        const auto contractPath = outputDirectory_ / "tensor-contract.txt";
        if (!std::filesystem::exists(contractPath))
        {
            std::ofstream contract{contractPath, std::ios::out | std::ios::trunc};
            contract << "dtype=float32\nlayout=NHWC\nshape="
                     << shape_[0] << ',' << shape_[1] << ','
                     << shape_[2] << ',' << shape_[3] << '\n';
        }
    }
    catch (...)
    {
        outputDirectory_.clear();
        limit_ = 0;
    }
}

void CalibrationTensorCapture::capture(
    const buffers::FloatTensorBuffer& tensor) noexcept
{
    if (outputDirectory_.empty() || captured_ >= limit_ || tensor.shape() != shape_)
    {
        return;
    }

    try
    {
        std::ostringstream name;
        name << processId_ << '-' << std::setw(6) << std::setfill('0')
             << captured_ << ".f32";
        const auto destination = outputDirectory_ / name.str();
        const auto temporary = destination.string() + ".tmp";
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        stream.write(
            reinterpret_cast<const char*>(tensor.data()),
            static_cast<std::streamsize>(tensor.elementCount() * sizeof(float)));
        stream.close();
        if (!stream)
        {
            std::filesystem::remove(temporary);
            outputDirectory_.clear();
            return;
        }
        std::filesystem::rename(temporary, destination);
        ++captured_;
    }
    catch (...)
    {
        outputDirectory_.clear();
    }
}
}
