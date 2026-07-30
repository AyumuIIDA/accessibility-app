#pragma once

#include <cstdint>

#if defined(_WIN32)
#define RYOIKI_EXPORT extern "C" __declspec(dllexport)
#else
#define RYOIKI_EXPORT extern "C"
#endif

struct RyoikiHandle;
struct RyoikiCadHandle;

inline constexpr std::uint32_t kRyoikiAbiVersion = 18;
inline constexpr std::int32_t kRyoikiStatusFailure = 0;
inline constexpr std::int32_t kRyoikiStatusSuccess = 1;
inline constexpr std::uint32_t kRyoikiMaxHandStates = 16;
inline constexpr std::uint32_t kRyoikiDomainExpansionStateId = 1;
inline constexpr std::uint32_t kRyoikiOpenPalmStateId = 2;
inline constexpr std::uint32_t kRyoikiMaxHandEventsPerBatch = 16;
inline constexpr std::uint32_t kRyoikiSwipeLeftEventId = 1;
inline constexpr std::uint32_t kRyoikiSwipeRightEventId = 2;

enum RyoikiHandPresentationMode : std::int32_t
{
    RYOIKI_HAND_PRESENTATION_MIRROR_DIRECT = 0,
    RYOIKI_HAND_PRESENTATION_PHYSICAL = 1
};

enum class RyoikiHandStatePhase : std::uint32_t
{
    Inactive = 0,
    Candidate = 1,
    Active = 2
};

enum class RyoikiHandStateTransition : std::uint32_t
{
    None = 0,
    Began = 1,
    Ended = 2,
    Cancelled = 3
};

struct RyoikiHandState
{
    std::uint32_t id;
    std::uint32_t phase;
    std::uint32_t transition;
    std::uint32_t flags;
    float confidence;
    float input_quality;
    std::uint32_t reserved0[2];
    std::uint64_t began_frame_id;
    std::uint64_t current_frame_id;
    std::uint64_t timestamp_us;
    std::uint32_t reserved[4];
};

struct RyoikiHandStateSnapshot
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t frame_id;
    std::uint64_t timestamp_us;
    std::uint32_t count;
    std::uint32_t flags;
    RyoikiHandState states[kRyoikiMaxHandStates];
};

static_assert(sizeof(RyoikiHandState) == 72);
static_assert(sizeof(RyoikiHandStateSnapshot) == 1184);

struct RyoikiHandEvent
{
    std::uint64_t sequence;
    std::uint32_t id;
    std::uint32_t flags;
    float confidence;
    float input_quality;
    float displacement_x;
    float displacement_y;
    std::uint64_t began_frame_id;
    std::uint64_t ended_frame_id;
    std::uint64_t began_timestamp_us;
    std::uint64_t ended_timestamp_us;
    std::uint64_t duration_us;
    std::uint32_t reserved[2];
};

struct RyoikiHandEventBatch
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t next_sequence;
    std::uint64_t dropped_count;
    std::uint32_t count;
    std::uint32_t flags;
    RyoikiHandEvent events[kRyoikiMaxHandEventsPerBatch];
};

static_assert(sizeof(RyoikiHandEvent) == 80);
static_assert(sizeof(RyoikiHandEventBatch) == 1312);

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
    double camera_upload_ms;
    double camera_draw_ms;
    double overlay_draw_ms;
    double hand_3d_draw_ms;
    double end_draw_ms;
    double present_wait_ms;
    double overlay_render_ms;
    double end_to_end_latency_ms;
    double native_overhead_ms;
    std::uint64_t frame_pool_dropped_frames;
    std::uint64_t perception_dropped_frames;
    std::uint64_t gpu_camera_frames;
    std::uint64_t gpu_rendered_frames;
    std::uint32_t gpu_camera_dxgi_format;
    std::uint32_t gpu_camera_subresource;
};

struct RyoikiHandResult
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t frame_id;
    std::uint64_t capture_timestamp_us;
    std::int32_t hand_count;
    float confidence;
    float handedness;
    float bbox[4];
    float landmarks[21 * 3];
    float world_landmarks[21 * 3];
    float palm_normal[3];
    // Column-major orthonormal palm basis: X across MCPs, Y wrist-to-middle,
    // Z palm normal. Translation and uniform hand scale are excluded.
    float palm_rotation_basis[3 * 3];
    float palm_center[3];
    float palm_scale;
    std::int32_t palm_pose_valid;
    // Upright image-space palm center. X and Y are normalized independently
    // to [0,1]. Scale is an isotropic upright-frame-width ratio.
    float screen_palm_center[2];
    float screen_palm_scale;
    std::int32_t screen_palm_valid;
    float palm_relative_rotation[3 * 3];
    float palm_rotation_fit_error;
    std::int32_t palm_relative_rotation_valid;
    float domain_sign_confidence;
    std::int32_t domain_sign_detected;
    float domain_sign_features[6];
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

struct RyoikiCadHandInteractionResult
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t frame_id;
    std::int32_t state;
    std::int32_t view_changed;
    float yaw_radians;
    float pitch_radians;
    float zoom;
    float pan_x;
    float pan_y;
    float yaw_delta_degrees;
    float pitch_delta_degrees;
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
RYOIKI_EXPORT std::int32_t ryoiki_get_latest_states(
    RyoikiHandle* handle,
    RyoikiHandStateSnapshot* out_snapshot);
RYOIKI_EXPORT std::int32_t ryoiki_read_hand_events(
    RyoikiHandle* handle,
    std::uint64_t after_sequence,
    RyoikiHandEventBatch* out_batch);
RYOIKI_EXPORT std::int32_t ryoiki_capture_palm_rotation_reference(RyoikiHandle* handle);
RYOIKI_EXPORT std::int32_t ryoiki_set_hand_presentation_mode(
    RyoikiHandle* handle,
    std::int32_t presentation_mode);
RYOIKI_EXPORT std::int32_t ryoiki_configure_cad_hand_interaction(
    RyoikiHandle* vision_handle,
    RyoikiCadHandle* cad_handle,
    std::int32_t interaction_mode,
    std::int32_t presentation_mode,
    float rotation_sensitivity);
RYOIKI_EXPORT std::int32_t ryoiki_get_cad_hand_interaction(
    RyoikiHandle* vision_handle,
    RyoikiCadHandInteractionResult* out_result);
RYOIKI_EXPORT std::int32_t ryoiki_get_last_error(
    RyoikiHandle* handle,
    char* buffer,
    std::int32_t buffer_length);
RYOIKI_EXPORT RyoikiCadHandle* ryoiki_cad_create(void* parent_hwnd);
RYOIKI_EXPORT std::int32_t ryoiki_cad_resize(
    RyoikiCadHandle* handle,
    std::int32_t width,
    std::int32_t height);
RYOIKI_EXPORT std::int32_t ryoiki_cad_set_view(
    RyoikiCadHandle* handle,
    float yaw_radians,
    float pitch_radians,
    float zoom);
RYOIKI_EXPORT std::int32_t ryoiki_cad_get_view(
    RyoikiCadHandle* handle,
    float* out_yaw_radians,
    float* out_pitch_radians,
    float* out_zoom);
RYOIKI_EXPORT std::int32_t ryoiki_cad_set_view_ex(
    RyoikiCadHandle* handle,
    float yaw_radians,
    float pitch_radians,
    float zoom,
    float pan_x,
    float pan_y);
RYOIKI_EXPORT std::int32_t ryoiki_cad_get_view_ex(
    RyoikiCadHandle* handle,
    float* out_yaw_radians,
    float* out_pitch_radians,
    float* out_zoom,
    float* out_pan_x,
    float* out_pan_y);
RYOIKI_EXPORT void ryoiki_cad_reset_view(RyoikiCadHandle* handle);
RYOIKI_EXPORT void ryoiki_cad_destroy(RyoikiCadHandle* handle);
RYOIKI_EXPORT std::int32_t ryoiki_cad_get_last_error(
    RyoikiCadHandle* handle,
    char* buffer,
    std::int32_t buffer_length);
