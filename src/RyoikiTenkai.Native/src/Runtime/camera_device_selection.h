#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace ryoiki::runtime
{
[[nodiscard]] std::size_t selectUserFacingCamera(
    std::span<const std::wstring> friendlyNames) noexcept;
}
