#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CameraCapture final
{
public:
    CameraCapture();
    ~CameraCapture();

    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;
    CameraCapture(CameraCapture&&) = delete;
    CameraCapture& operator=(CameraCapture&&) = delete;

    bool initialize(std::string& error);
    void requestStop() noexcept;
    bool readFrame(
        std::vector<std::uint8_t>& bgra,
        std::uint32_t& width,
        std::uint32_t& height,
        double& cameraWaitMs,
        double& frameCopyMs,
        std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
