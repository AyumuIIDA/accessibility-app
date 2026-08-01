#pragma once

#include <cstdint>

#if defined(_WIN32)
#define RYOIKI_EXPORT extern "C" __declspec(dllexport)
#else
#define RYOIKI_EXPORT extern "C"
#endif

struct RyoikiHandle;
struct RyoikiCadHandle;
struct RyoikiGestureDtwDebugHandle;
struct RyoikiGestureRecordingPlaybackHandle;

inline constexpr std::uint32_t kRyoikiAbiVersion = 29;
inline constexpr std::int32_t kRyoikiStatusFailure = 0;
inline constexpr std::int32_t kRyoikiStatusSuccess = 1;
inline constexpr std::uint32_t kRyoikiMaxHands = 2;
inline constexpr std::uint32_t kRyoikiMaxHandStates = 16;
inline constexpr std::uint32_t kRyoikiDomainExpansionStateId = 1;
inline constexpr std::uint32_t kRyoikiMaxHandEventsPerBatch = 16;

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

struct RyoikiObservedHand
{
    std::uint32_t track_id;
    // Bit 0: landmark came from an existing tracking ROI.
    std::uint32_t flags;
    float confidence;
    float handedness;
    float filtered_handedness;
    float bbox[4];
    float landmarks[21 * 3];
    float world_landmarks[21 * 3];
};

struct RyoikiHandsResult
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t frame_id;
    std::uint64_t capture_timestamp_us;
    std::uint32_t hand_count;
    // Bit 0: two-track association is currently ambiguous/occluded.
    std::uint32_t flags;
    std::uint32_t crossing_owner_track_id;
    std::uint32_t reserved;
    RyoikiObservedHand hands[kRyoikiMaxHands];
};

static_assert(sizeof(RyoikiObservedHand) == 540);
static_assert(sizeof(RyoikiHandsResult) == 1120);

enum RyoikiTwoHandOrdering : std::uint32_t
{
    RYOIKI_TWO_HAND_ORDERING_NONE = 0,
    RYOIKI_TWO_HAND_ORDERING_HANDEDNESS = 1,
    RYOIKI_TWO_HAND_ORDERING_SCREEN_POSITION = 2
};

// Bounded temporal topology summary for one tracked hand over the rolling
// 2600 ms measurement window. Lengths are expressed in palm-axis lengths
// (wrist to middle MCP), so every value is dimensionless and independent of
// resolution and camera distance. Angles are radians in the upright image.
struct RyoikiHandTopologySummary
{
    std::uint32_t track_id;
    std::uint32_t valid;
    std::uint32_t sample_count;
    std::uint32_t finger_state_transition_count;
    // Bit i is set when finger i (thumb..pinky) is extended.
    std::uint32_t start_finger_state_mask;
    std::uint32_t end_finger_state_mask;
    std::uint32_t signed_palm_area_sign_changes;
    std::uint32_t reserved;
    std::uint64_t first_frame_id;
    std::uint64_t last_frame_id;
    std::uint64_t duration_us;
    // Accumulated wrist path length, not endpoint distance.
    float palm_travel;
    float palm_orientation_range_radians;
    float handedness_range;
    float handedness_mean;
    float finger_straightness_range_max;
    float signed_palm_area_range;
    float palm_compression_min;
    float palm_compression_max;
    float palm_compression_drop;
    float palm_depth_range_max;
    float palm_turn_score;
    float hand_scale_ratio_range;
    float hand_scale_ratio_delta;
    float bounding_box_area_ratio_range;
    float bounding_box_area_ratio_delta;
    float translation_delta_x;
    float translation_delta_y;
    float translation_distance;
    float topology_change_score;
    float reserved_float;
};

// Relative geometry of the two observed hands for one frame, in palm-axis
// lengths. Ordering is a deterministic presentation of the pair; identity
// remains the perception track ID.
struct RyoikiTwoHandRelation
{
    std::uint32_t valid;
    std::uint32_t first_track_id;
    std::uint32_t second_track_id;
    // RyoikiTwoHandOrdering.
    std::uint32_t ordering;
    float delta_x;
    float delta_y;
    float distance;
    float angle_radians;
    float scale_ratio;
    float quality;
};

struct RyoikiHandTopologySnapshot
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t frame_id;
    std::uint64_t capture_timestamp_us;
    std::uint32_t summary_count;
    std::uint32_t reserved;
    RyoikiHandTopologySummary summaries[kRyoikiMaxHands];
    RyoikiTwoHandRelation relation;
};

static_assert(sizeof(RyoikiHandTopologySummary) == 136);
static_assert(sizeof(RyoikiTwoHandRelation) == 40);
static_assert(sizeof(RyoikiHandTopologySnapshot) == 344);

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

// Latest-only, single-track, on-demand gesture-recognition diagnostic
// snapshot (ABI version 22+). Published by a throttled recognition pass on
// the perception worker (not run every frame) that iterates the current
// primary track's rolling candidate-window history against the in-memory
// registered template set; see doc/hand-input-architecture.md and
// doc/gesture-recognition-framework.md for the Measurement -> Recognition
// boundary and native HandInput/Recognition/gesture_template_registry.*.
// This struct is deliberately small: no landmark or per-frame feature
// buffers cross the ABI, only scalar diagnostics for the single best-scoring
// (template, candidate duration) attempt of the most recent recognition pass,
// plus the most recent on-demand template-registration outcome. Intended for
// real-device validation logging, not action execution or event publication.
struct RyoikiGestureRecognitionSnapshot
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t frame_id;
    std::uint64_t timestamp_us;
    std::uint64_t best_candidate_start_timestamp_us;
    std::uint64_t best_candidate_end_timestamp_us;
    // Native stable track ID the diagnostic covers; 0 when no track was
    // evaluated this pass.
    std::uint32_t track_id;
    // Nonzero when a recognition pass actually ran and evaluated at least
    // one candidate window this poll interval (independent of whether it
    // matched).
    std::uint32_t has_result;
    std::uint32_t template_count;
    // Candidate windows built (0..4) for the evaluated track this pass.
    std::uint32_t candidate_count;
    std::uint32_t best_template_id;
    std::uint32_t best_candidate_duration_ms;
    std::uint32_t best_eligible;
    std::uint32_t best_usable_frame_count;
    std::uint32_t best_source_frame_count;
    std::uint32_t best_active_segment_valid;
    float best_score;
    float best_confidence;
    float best_warp_ratio;
    std::uint32_t best_has_warp_ratio;
    float best_reverse_score;
    std::uint32_t best_has_reverse_score;
    float best_active_segment_path_velocity;
    // Outcome of the most recent ryoiki_request_gesture_template_registration
    // call: 0 = none requested yet, 1 = success, 2 = rejected (see
    // last_registration_reason), 3 = no candidate history for the requested
    // track.
    std::uint32_t last_registration_status;
    std::uint32_t last_registration_template_id;
    char best_rejection_reason[128];
    char last_registration_reason[128];
    std::uint32_t reserved[4];
};

// Explicit gesture-recording session lifecycle (ABI version 23+). Replaces
// the ryoiki_request_gesture_template_registration stop-time path for the
// application's recording workflow: ryoiki_begin_gesture_recording,
// ryoiki_finish_gesture_recording, and ryoiki_cancel_gesture_recording are
// the only functions the UI calls to drive one take, and this snapshot is
// the only function it polls for progress/outcome. The UI never supplies a
// track ID; see doc/hand-input-architecture.md and
// doc/native-vision-runtime.md for the Begin/Recording/Finish contract.
enum RyoikiGestureRecordingState : std::uint32_t
{
    RYOIKI_GESTURE_RECORDING_IDLE = 0,
    RYOIKI_GESTURE_RECORDING_AWAITING_HAND = 1,
    RYOIKI_GESTURE_RECORDING_RECORDING = 2,
    RYOIKI_GESTURE_RECORDING_COMPLETED = 3,
    RYOIKI_GESTURE_RECORDING_REJECTED = 4,
    RYOIKI_GESTURE_RECORDING_CANCELLED = 5,
    RYOIKI_GESTURE_RECORDING_TRACK_LOST = 6,
    RYOIKI_GESTURE_RECORDING_AWAITING_NEXT_TAKE = 7,
};

struct RyoikiGestureRecordingStatus
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    // RyoikiGestureRecordingState.
    std::uint32_t state;
    // The perception-graph track the session locked onto at Begin; 0 before
    // a track is locked. Diagnostic only - the caller never selects this.
    std::uint32_t track_id;
    std::uint64_t began_frame_id;
    std::uint64_t began_timestamp_us;
    std::uint64_t last_frame_id;
    std::uint64_t last_timestamp_us;
    std::uint32_t sample_count;
    std::uint32_t usable_sample_count;
    // The template_id supplied to ryoiki_begin_gesture_recording, echoed
    // once ryoiki_finish_gesture_recording resolves (Completed or
    // Rejected); 0 before any Finish attempt this take.
    std::uint32_t last_result_template_id;
    std::uint32_t current_take;
    char rejection_reason[128];
    std::uint32_t accepted_take_count;
    std::uint32_t required_take_count;
    std::uint32_t attempt_count;
    std::uint32_t reserved[1];
};

static_assert(sizeof(RyoikiGestureRecordingStatus) <= 256);

inline constexpr std::uint32_t kRyoikiMaxGestureDefinitions = 8;
struct RyoikiGestureDefinitionMetadata { std::uint32_t id; std::uint32_t enabled; std::uint32_t take_count; char name[64]; };
struct RyoikiGestureDefinitionList { std::uint32_t abi_version; std::uint32_t struct_size; std::uint32_t count; RyoikiGestureDefinitionMetadata items[kRyoikiMaxGestureDefinitions]; char error[128]; };
inline constexpr std::uint32_t kRyoikiMaxGestureRecordings = 3;
struct RyoikiGestureRecordingMetadata
{
    std::uint32_t take_index; std::uint32_t accepted; std::uint64_t captured_at_us;
    double duration_ms; float average_confidence;
    std::uint32_t source_frame_count; std::uint32_t valid_landmark_frame_count;
    std::uint32_t high_confidence_frame_count; std::uint32_t frame_count;
    float effective_fps; float usable_effective_fps;
    char source_id[64]; char quality_reason[128];
};
struct RyoikiGestureRecordingList
{
    std::uint32_t abi_version; std::uint32_t struct_size; std::uint32_t count; std::uint32_t reserved0;
    RyoikiGestureRecordingMetadata items[kRyoikiMaxGestureRecordings]; char error[128];
};
struct RyoikiGesturePlaybackStatus
{
    std::uint32_t abi_version; std::uint32_t struct_size; std::uint32_t definition_id;
    std::uint32_t take_index; std::uint32_t playing; std::uint32_t frame_index;
    std::uint32_t frame_count; float normalized_progress; double duration_ms; double position_ms;
};
static_assert(sizeof(RyoikiGestureRecordingMetadata) == 248);
static_assert(sizeof(RyoikiGestureRecordingList) == 888);
struct RyoikiGestureBindingMetadata { std::uint32_t definition_id; std::uint32_t enabled; char action_type[32]; char action_parameter[256]; };
struct RyoikiGestureBindingList { std::uint32_t abi_version; std::uint32_t struct_size; std::uint32_t count; RyoikiGestureBindingMetadata items[kRyoikiMaxGestureDefinitions]; char error[128]; };

inline constexpr std::uint32_t kRyoikiMaxGestureDtwPathPoints = 64;

struct RyoikiGestureDtwDebugSnapshot
{
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    std::uint64_t frame_id;
    std::uint64_t timestamp_us;
    std::uint32_t template_id;
    std::uint32_t candidate_duration_ms;
    std::uint32_t eligible;
    std::uint32_t path_count;
    float score;
    float confidence;
    float threshold_score;
    float warp_ratio;
    float reverse_score;
    std::uint32_t has_warp_ratio;
    std::uint32_t has_reverse_score;
    std::uint32_t reserved0;
    // joint, bone, curl, finger state, spacing, motion, palm turn, depth,
    // handedness, size, translation, and total.
    float score_breakdown[12];
    std::uint8_t candidate_indices[kRyoikiMaxGestureDtwPathPoints];
    std::uint8_t template_indices[kRyoikiMaxGestureDtwPathPoints];
    char rejection_reason[128];
    std::uint32_t reserved[4];
};

static_assert(sizeof(RyoikiGestureDtwDebugSnapshot) <= 512);

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
RYOIKI_EXPORT std::int32_t ryoiki_get_latest_hands(
    RyoikiHandle* handle,
    RyoikiHandsResult* out_result);
RYOIKI_EXPORT std::int32_t ryoiki_get_latest_hand_topology(
    RyoikiHandle* handle,
    RyoikiHandTopologySnapshot* out_snapshot);
RYOIKI_EXPORT std::int32_t ryoiki_get_latest_states(
    RyoikiHandle* handle,
    RyoikiHandStateSnapshot* out_snapshot);
RYOIKI_EXPORT std::int32_t ryoiki_read_hand_events(
    RyoikiHandle* handle,
    std::uint64_t after_sequence,
    RyoikiHandEventBatch* out_batch);
RYOIKI_EXPORT std::int32_t ryoiki_get_latest_gesture_recognition(
    RyoikiHandle* handle,
    RyoikiGestureRecognitionSnapshot* out_snapshot);
RYOIKI_EXPORT std::int32_t ryoiki_get_latest_gesture_dtw_debug(
    RyoikiHandle* handle,
    RyoikiGestureDtwDebugSnapshot* out_snapshot);
// Requests that the perception worker register a new in-memory gesture
// template (or replace an existing one, if template_id is already
// registered) from the requested track's current rolling candidate-window
// history the next time it processes a frame. Fire-and-forget, mirroring
// ryoiki_capture_palm_rotation_reference: this call only sets a request flag
// and returns immediately (kRyoikiStatusSuccess if the handle is valid), it
// does not itself touch the candidate history (which is perception-worker-
// owned and not safe to read from another thread). Poll
// ryoiki_get_latest_gesture_recognition's last_registration_* fields for the
// outcome. track_id == 0 requests the current primary track (the most
// recently observed hand's track).
RYOIKI_EXPORT std::int32_t ryoiki_request_gesture_template_registration(
    RyoikiHandle* handle,
    std::uint32_t track_id,
    std::uint32_t template_id);
RYOIKI_EXPORT std::int32_t ryoiki_list_gesture_definitions(RyoikiHandle*,RyoikiGestureDefinitionList*);
RYOIKI_EXPORT std::int32_t ryoiki_set_gesture_definition_metadata(RyoikiHandle*,std::uint32_t,const char*,std::uint32_t);
RYOIKI_EXPORT std::int32_t ryoiki_delete_gesture_definition(RyoikiHandle*,std::uint32_t);
RYOIKI_EXPORT std::int32_t ryoiki_reload_gesture_definitions(RyoikiHandle*);
RYOIKI_EXPORT std::int32_t ryoiki_list_gesture_recordings(
    RyoikiHandle*, std::uint32_t, RyoikiGestureRecordingList*);
RYOIKI_EXPORT std::int32_t ryoiki_export_gesture_recording(
    RyoikiHandle*, std::uint32_t, std::uint32_t, const char*);
RYOIKI_EXPORT std::int32_t ryoiki_list_gesture_bindings(RyoikiHandle*,RyoikiGestureBindingList*);
RYOIKI_EXPORT std::int32_t ryoiki_upsert_gesture_binding(RyoikiHandle*,std::uint32_t,const char*,const char*,std::uint32_t);
RYOIKI_EXPORT std::int32_t ryoiki_delete_gesture_binding(RyoikiHandle*,std::uint32_t);
// Starts a new explicit gesture-recording take (ABI version 23+). Fire-
// and-forget, like ryoiki_capture_palm_rotation_reference: this only
// enqueues a command for the perception worker and returns immediately
// (kRyoikiStatusSuccess if the handle is valid and running). The worker
// waits for exactly one visible, usable hand and locks the session onto
// that hand's native track; the caller never supplies a track ID.
// template_id is the caller-assigned ID this take will be registered under
// once ryoiki_finish_gesture_recording succeeds - remembered from this call,
// not repeated at Finish. Calling this again while a take is in progress
// abandons it and starts a fresh one. Poll
// ryoiki_get_gesture_recording_status for progress and outcome.
RYOIKI_EXPORT std::int32_t ryoiki_begin_gesture_recording(
    RyoikiHandle* handle,
    std::uint32_t template_id);
// Ends the current take and attempts to build/register a template from
// exactly the samples recorded since ryoiki_begin_gesture_recording, using
// the template_id supplied to that call. Fire-and-forget; poll
// ryoiki_get_gesture_recording_status's state/rejection_reason for the
// outcome (RYOIKI_GESTURE_RECORDING_COMPLETED or _REJECTED). A call outside
// an active take is a no-op that still returns success.
RYOIKI_EXPORT std::int32_t ryoiki_finish_gesture_recording(RyoikiHandle* handle);
// Abandons the current take without registering a template. Fire-and-
// forget; a call outside an active take is a no-op that still returns
// success.
RYOIKI_EXPORT std::int32_t ryoiki_cancel_gesture_recording(RyoikiHandle* handle);
RYOIKI_EXPORT std::int32_t ryoiki_get_gesture_recording_status(
    RyoikiHandle* handle,
    RyoikiGestureRecordingStatus* out_status);
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
RYOIKI_EXPORT RyoikiGestureDtwDebugHandle* ryoiki_dtw_debug_create(
    void* parent_hwnd, RyoikiHandle* source);
RYOIKI_EXPORT std::int32_t ryoiki_dtw_debug_resize(
    RyoikiGestureDtwDebugHandle* handle, std::int32_t width, std::int32_t height);
RYOIKI_EXPORT std::int32_t ryoiki_dtw_debug_get_last_error(
    RyoikiGestureDtwDebugHandle* handle, char* buffer, std::int32_t length);
RYOIKI_EXPORT void ryoiki_dtw_debug_destroy(RyoikiGestureDtwDebugHandle* handle);
RYOIKI_EXPORT RyoikiGestureRecordingPlaybackHandle* ryoiki_recording_playback_create(
    void* parent_hwnd, RyoikiHandle* source);
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_resize(
    RyoikiGestureRecordingPlaybackHandle*, std::int32_t, std::int32_t);
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_select(
    RyoikiGestureRecordingPlaybackHandle*, std::uint32_t, std::uint32_t);
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_set_playing(
    RyoikiGestureRecordingPlaybackHandle*, std::uint32_t);
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_seek(
    RyoikiGestureRecordingPlaybackHandle*, float);
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_get_status(
    RyoikiGestureRecordingPlaybackHandle*, RyoikiGesturePlaybackStatus*);
RYOIKI_EXPORT std::int32_t ryoiki_recording_playback_get_last_error(
    RyoikiGestureRecordingPlaybackHandle*, char*, std::int32_t);
RYOIKI_EXPORT void ryoiki_recording_playback_destroy(RyoikiGestureRecordingPlaybackHandle*);
RYOIKI_EXPORT std::int32_t ryoiki_cad_get_last_error(
    RyoikiCadHandle* handle,
    char* buffer,
    std::int32_t buffer_length);
