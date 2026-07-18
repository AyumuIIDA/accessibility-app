#pragma once

#include "Runtime/frame_orientation.h"
#include "Runtime/d3d11_device.h"

#include <d3d11.h>
#include <wrl/client.h>

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

    bool initialize(std::shared_ptr<ryoiki::runtime::D3d11Device> d3dDevice, std::string& error);
    void requestStop() noexcept;
    bool readFrame(
        std::vector<std::uint8_t>& bgra,
        std::uint32_t& width,
        std::uint32_t& height,
        ryoiki::runtime::FrameRotation& orientation,
        double& cameraWaitMs,
        double& frameCopyMs,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& gpuTexture,
        std::uint32_t& gpuTextureSubresource,
        Microsoft::WRL::ComPtr<IUnknown>& gpuSampleOwner,
        bool copyToCpu,
        std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
