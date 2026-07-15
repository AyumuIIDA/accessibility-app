#include "Runtime/camera_device_selection.h"

#include <algorithm>
#include <cwctype>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace ryoiki::runtime
{
namespace
{
bool containsAny(
    const std::wstring_view value,
    const std::initializer_list<std::wstring_view> tokens)
{
    return std::ranges::any_of(tokens, [value](const std::wstring_view token)
    {
        return value.find(token) != std::wstring_view::npos;
    });
}

int cameraPreferenceScore(const std::wstring& friendlyName)
{
    std::wstring normalized = friendlyName;
    std::ranges::transform(normalized, normalized.begin(), [](const wchar_t character)
    {
        return static_cast<wchar_t>(std::towlower(character));
    });

    int score = 0;
    if (containsAny(normalized, {L"front", L"user facing", L"user-facing"}))
    {
        score += 100;
    }
    if (containsAny(normalized, {L"rear", L"back", L"world facing", L"world-facing"}))
    {
        score -= 100;
    }
    if (containsAny(normalized, {L"infrared", L" ir ", L"depth"}))
    {
        score -= 50;
    }
    return score;
}
}

std::size_t selectUserFacingCamera(
    const std::span<const std::wstring> friendlyNames) noexcept
{
    std::size_t selectedIndex = 0;
    int selectedScore = std::numeric_limits<int>::min();
    for (std::size_t index = 0; index < friendlyNames.size(); ++index)
    {
        const int score = cameraPreferenceScore(friendlyNames[index]);
        if (score > selectedScore)
        {
            selectedIndex = index;
            selectedScore = score;
        }
    }
    return selectedIndex;
}
}
