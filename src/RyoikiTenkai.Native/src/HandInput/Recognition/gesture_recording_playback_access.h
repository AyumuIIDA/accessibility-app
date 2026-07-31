#pragma once
#include "HandInput/Recognition/gesture_template_repository.h"
#include <cstdint>
#include <string>
struct RyoikiHandle;
[[nodiscard]] bool ryoikiLoadGestureRecording(
    RyoikiHandle*, std::uint32_t, std::uint32_t,
    ryoiki::hand_input::recognition::GestureRecordingProvenance&, std::string&) noexcept;
