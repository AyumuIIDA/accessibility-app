#pragma once

#include <cstdint>

#if defined(_WIN32)
#define RYOIKI_EXPORT extern "C" __declspec(dllexport)
#else
#define RYOIKI_EXPORT extern "C"
#endif

struct RyoikiHandle;

inline constexpr std::uint32_t kRyoikiAbiVersion = 4;
inline constexpr std::int32_t kRyoikiStatusFailure = 0;
inline constexpr std::int32_t kRyoikiStatusSuccess = 1;

struct RyoikiMetrics
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t frame_id;
    std::uint64_t capture_timestamp_us;
    double runtime_seconds;
    double camera_fps;
    double display_fps;
    double perception_fps;
    double camera_wait_ms;
    double frame_copy_ms;
    double preprocess_ms;
    double palm_inference_ms;
    double palm_postprocess_ms;
    double roi_crop_warp_ms;
    double hand_inference_ms;
    double landmark_postprocess_ms;
    double tracking_update_ms;
    double overlay_render_ms;
    double end_to_end_latency_ms;
    double native_overhead_ms;
    std::uint64_t frame_pool_dropped_frames;
    std::uint64_t perception_dropped_frames;
};

struct RyoikiHandResult
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t frame_id;
    std::int32_t hand_count;
    float confidence;
    float handedness;
    float bbox[4];
    float landmarks[21 * 3];
};

struct RyoikiPalmResult
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t frame_id;
    std::int32_t palm_count;
    float confidence;
    float bbox[4];
    float keypoints[7 * 2];
};

RYOIKI_EXPORT std::uint32_t ryoiki_get_abi_version();
RYOIKI_EXPORT RyoikiHandle* ryoiki_create(void* parent_hwnd);
RYOIKI_EXPORT std::int32_t ryoiki_start(RyoikiHandle* handle);
RYOIKI_EXPORT void ryoiki_stop(RyoikiHandle* handle);
RYOIKI_EXPORT std::int32_t ryoiki_resize(
    RyoikiHandle* handle,
    std::int32_t width,
    std::int32_t height);
RYOIKI_EXPORT void ryoiki_destroy(RyoikiHandle* handle);
RYOIKI_EXPORT std::int32_t ryoiki_get_latest_metrics(RyoikiHandle* handle, RyoikiMetrics* out_metrics);
RYOIKI_EXPORT std::int32_t ryoiki_get_latest_palm(RyoikiHandle* handle, RyoikiPalmResult* out_result);
RYOIKI_EXPORT std::int32_t ryoiki_get_latest_hand(RyoikiHandle* handle, RyoikiHandResult* out_result);
RYOIKI_EXPORT std::int32_t ryoiki_get_last_error(
    RyoikiHandle* handle,
    char* buffer,
    std::int32_t buffer_length);
