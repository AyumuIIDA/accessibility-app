#pragma once
#include "HandInput/Recognition/multi_hand_gesture_core.h"
#include "HandInput/Recognition/gesture_template_repository.h"
#include "HandInput/Recognition/unified_sequence_gesture_comparator.h"
#include <cstdint>
#include <vector>
namespace ryoiki::hand_input::recognition {
inline constexpr std::uint32_t kGestureTemplateFormatVersion = 1;
[[nodiscard]] std::vector<std::uint8_t> serializeGestureTemplate(const UnifiedSequenceTemplate& value);
[[nodiscard]] bool deserializeGestureTemplate(const std::uint8_t* data, std::size_t size,
    UnifiedSequenceTemplate& value) noexcept;
[[nodiscard]] std::vector<std::uint8_t> serializeTwoHandTemplate(const TwoHandTemplate& value);
[[nodiscard]] bool deserializeTwoHandTemplate(const std::uint8_t* data, std::size_t size,
    TwoHandTemplate& value) noexcept;
[[nodiscard]] std::vector<std::uint8_t> serializeGestureRecording(const GestureRecordingProvenance& value);
[[nodiscard]] bool deserializeGestureRecording(const std::uint8_t* data, std::size_t size,
    GestureRecordingProvenance& value) noexcept;
}
