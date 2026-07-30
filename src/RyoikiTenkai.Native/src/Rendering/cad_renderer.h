#pragma once

#include "Rendering/cad_view.h"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ryoiki::runtime { class D3d11Device; }

namespace ryoiki::rendering
{
class CadRenderer final
{
public:
    CadRenderer();
    ~CadRenderer();
    CadRenderer(const CadRenderer&) = delete;
    CadRenderer& operator=(const CadRenderer&) = delete;

    [[nodiscard]] bool initialize(
        std::shared_ptr<runtime::D3d11Device> device,
        HWND hwnd,
        std::uint32_t width,
        std::uint32_t height,
        std::string& error);
    [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height, std::string& error);
    void setView(CadView view) noexcept;
    [[nodiscard]] bool render(std::string& error);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
