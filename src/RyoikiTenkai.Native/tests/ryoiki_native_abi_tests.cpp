// Deterministic layout checks for the C ABI surface. This file intentionally
// includes only include/ryoiki_native.h (the public ABI header, no camera or
// HWND dependency) so it can run without a live RyoikiHandle - ryoiki_create
// touches Win32/D3D11/camera capture and is not unit-testable here.
#include "ryoiki_native.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace
{
void require(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error{message};
    }
}

void testAbiVersionIsCurrent()
{
    require(kRyoikiAbiVersion == 29, "kRyoikiAbiVersion must be bumped when the ABI contract changes.");
}

void testGestureRecordingPlaybackLayout()
{
    using Metadata = RyoikiGestureRecordingMetadata;
    using List = RyoikiGestureRecordingList;
    using Status = RyoikiGesturePlaybackStatus;
    require(std::is_standard_layout_v<Metadata> && std::is_trivially_copyable_v<Metadata>,
        "Gesture recording metadata must remain a copyable C ABI value.");
    require(sizeof(Metadata) == 248, "Gesture recording metadata no longer matches the managed ABI mirror.");
    require(offsetof(List, reserved0) == sizeof(std::uint32_t) * 3
            && offsetof(List, items) == sizeof(std::uint32_t) * 4,
        "Gesture recording list alignment no longer matches the managed ABI mirror.");
    require(sizeof(List) == 888, "Gesture recording list no longer matches the managed ABI mirror.");
    require(std::is_standard_layout_v<Status> && std::is_trivially_copyable_v<Status>
            && sizeof(Status) <= 64,
        "Playback status must remain bounded copyable metadata.");
}

void testGestureRecordingStatusLayout()
{
    using Status = RyoikiGestureRecordingStatus;
    require(std::is_standard_layout_v<Status> && std::is_trivially_copyable_v<Status>,
        "RyoikiGestureRecordingStatus must be a copyable C ABI value.");
    require(offsetof(Status, abi_version) == 0
            && offsetof(Status, struct_size) == sizeof(std::uint32_t),
        "Recording status must use the common ABI header.");
    require(sizeof(Status) <= 256,
        "Recording status must remain small metadata, not a frame buffer.");
    Status status{};
    require(offsetof(Status, current_take) < offsetof(Status, accepted_take_count)
            && offsetof(Status, accepted_take_count) < offsetof(Status, required_take_count)
            && offsetof(Status, required_take_count) < offsetof(Status, attempt_count),
        "Three-take progress metadata must remain ordered in the ABI snapshot.");
    require(status.rejection_reason[0] == '\0',
        "A zero-initialized recording rejection reason must be an empty C string.");
}

void testGestureDefinitionListLayout()
{
    using Metadata = RyoikiGestureDefinitionMetadata;
    using List = RyoikiGestureDefinitionList;
    require(std::is_standard_layout_v<Metadata> && std::is_trivially_copyable_v<Metadata>,
        "Gesture definition metadata must remain a copyable C ABI value.");
    require(sizeof(Metadata) == 76,
        "Gesture definition metadata no longer matches the managed ABI mirror.");
    require(offsetof(List, abi_version) == 0
            && offsetof(List, struct_size) == sizeof(std::uint32_t)
            && offsetof(List, count) == sizeof(std::uint32_t) * 2,
        "Gesture definition list must use the common ABI header without hidden fields.");
    require(sizeof(List) == 748,
        "Gesture definition list no longer matches the managed ABI mirror.");
    List list{};
    require(sizeof(list.items) / sizeof(list.items[0]) == kRyoikiMaxGestureDefinitions,
        "Gesture definition list capacity drifted from its public maximum.");
    require(list.error[0] == '\0' && list.items[0].name[0] == '\0',
        "Zero-initialized repository diagnostics and names must be empty strings.");
}

void testGestureDtwDebugSnapshotLayout()
{
    using Snapshot = RyoikiGestureDtwDebugSnapshot;
    require(std::is_standard_layout_v<Snapshot> && std::is_trivially_copyable_v<Snapshot>,
        "RyoikiGestureDtwDebugSnapshot must remain a copyable C ABI value.");
    require(offsetof(Snapshot, abi_version) == 0
            && offsetof(Snapshot, struct_size) == sizeof(std::uint32_t),
        "DTW debug snapshot must use the common ABI header.");
    require(sizeof(Snapshot) <= 512,
        "DTW debug snapshot must remain bounded metadata.");
    Snapshot snapshot{};
    require(sizeof(snapshot.candidate_indices) == kRyoikiMaxGestureDtwPathPoints
            && sizeof(snapshot.template_indices) == kRyoikiMaxGestureDtwPathPoints,
        "DTW path buffers drifted from the public maximum.");
    require(snapshot.rejection_reason[0] == '\0',
        "A zero-initialized DTW rejection reason must be empty.");
}

void testGestureRecognitionSnapshotIsTriviallyCopyablePod()
{
    require(std::is_standard_layout_v<RyoikiGestureRecognitionSnapshot>,
        "RyoikiGestureRecognitionSnapshot must be standard-layout to cross the C ABI safely.");
    require(std::is_trivially_copyable_v<RyoikiGestureRecognitionSnapshot>,
        "RyoikiGestureRecognitionSnapshot must be trivially copyable (no ownership, no destructor work) "
        "to cross the C ABI safely.");
}

void testGestureRecognitionSnapshotStaysSmall()
{
    // "SMALL diagnostic snapshot": no landmark arrays, no per-frame feature
    // vectors, no per-attempt lists cross the ABI - only fixed scalar
    // diagnostics for the single best match plus two short reason strings.
    require(sizeof(RyoikiGestureRecognitionSnapshot) <= 512,
        "The gesture-recognition snapshot must stay small; a growth here likely means a large buffer leaked "
        "across the ABI boundary.");
    require(sizeof(RyoikiGestureRecognitionSnapshot) % alignof(RyoikiGestureRecognitionSnapshot) == 0,
        "The struct size must be a multiple of its own alignment (no silent implicit padding surprises).");
}

void testGestureRecognitionSnapshotHeaderFieldsMatchOtherSnapshots()
{
    // Every latest-only snapshot in this ABI starts with the same
    // (abi_version, struct_size) header pair at offset 0/4, matching
    // RyoikiMetrics/RyoikiHandResult/RyoikiHandsResult/
    // RyoikiHandTopologySnapshot/RyoikiHandStateSnapshot/RyoikiHandEventBatch,
    // so callers can validate any snapshot the same way.
    require(offsetof(RyoikiGestureRecognitionSnapshot, abi_version) == 0,
        "abi_version must be the first field.");
    require(offsetof(RyoikiGestureRecognitionSnapshot, struct_size) == sizeof(std::uint32_t),
        "struct_size must immediately follow abi_version.");
}

void testGestureRecognitionSnapshotFieldOffsetsAreMonotonicAndNonOverlapping()
{
    // Declaration order in ryoiki_native.h is the load-bearing contract for
    // this test: each field's offset must be >= the previous field's offset
    // plus its own size (no accidental reordering/overlap introduced by a
    // future edit).
    using Snapshot = RyoikiGestureRecognitionSnapshot;
    const std::size_t offsets[] = {
        offsetof(Snapshot, abi_version),
        offsetof(Snapshot, struct_size),
        offsetof(Snapshot, frame_id),
        offsetof(Snapshot, timestamp_us),
        offsetof(Snapshot, best_candidate_start_timestamp_us),
        offsetof(Snapshot, best_candidate_end_timestamp_us),
        offsetof(Snapshot, track_id),
        offsetof(Snapshot, has_result),
        offsetof(Snapshot, template_count),
        offsetof(Snapshot, candidate_count),
        offsetof(Snapshot, best_template_id),
        offsetof(Snapshot, best_candidate_duration_ms),
        offsetof(Snapshot, best_eligible),
        offsetof(Snapshot, best_usable_frame_count),
        offsetof(Snapshot, best_source_frame_count),
        offsetof(Snapshot, best_active_segment_valid),
        offsetof(Snapshot, best_score),
        offsetof(Snapshot, best_confidence),
        offsetof(Snapshot, best_warp_ratio),
        offsetof(Snapshot, best_has_warp_ratio),
        offsetof(Snapshot, best_reverse_score),
        offsetof(Snapshot, best_has_reverse_score),
        offsetof(Snapshot, best_active_segment_path_velocity),
        offsetof(Snapshot, last_registration_status),
        offsetof(Snapshot, last_registration_template_id),
        offsetof(Snapshot, best_rejection_reason),
        offsetof(Snapshot, last_registration_reason),
        offsetof(Snapshot, reserved),
    };
    for (std::size_t i = 1; i < std::size(offsets); ++i)
    {
        require(offsets[i] > offsets[i - 1],
            "Declared fields must have strictly increasing offsets (no reordering/overlap).");
    }
    require(offsetof(Snapshot, reserved) + sizeof(Snapshot::reserved) <= sizeof(Snapshot),
        "The trailing reserved block must fit entirely inside the struct's own size.");
}

void testGestureRecognitionSnapshotReasonBuffersAreFixedAndNulTerminatable()
{
    RyoikiGestureRecognitionSnapshot snapshot{};
    require(sizeof(snapshot.best_rejection_reason) == 128, "best_rejection_reason must stay a fixed 128 bytes.");
    require(sizeof(snapshot.last_registration_reason) == 128,
        "last_registration_reason must stay a fixed 128 bytes.");
    // A zero-initialized snapshot must already be a valid (empty) C string
    // in each reason buffer, so an unpopulated/empty-result poll never
    // exposes uninitialized memory to a caller that reads it as text.
    require(snapshot.best_rejection_reason[0] == '\0', "A zero-initialized snapshot's reason must be empty.");
    require(snapshot.last_registration_reason[0] == '\0', "A zero-initialized snapshot's reason must be empty.");
}

void testStructSizesUnchangedForExistingAbiStructs()
{
    // Regression guard: this file only includes the header, so a future
    // accidental change to an existing struct's layout fails here too, not
    // only via the header's own static_asserts.
    require(sizeof(RyoikiHandState) == 72, "RyoikiHandState layout must stay stable across this ABI version.");
    require(sizeof(RyoikiHandStateSnapshot) == 1184, "RyoikiHandStateSnapshot layout must stay stable.");
    require(sizeof(RyoikiHandEvent) == 80, "RyoikiHandEvent layout must stay stable.");
    require(sizeof(RyoikiHandEventBatch) == 1312, "RyoikiHandEventBatch layout must stay stable.");
    require(sizeof(RyoikiHandTopologySummary) == 136, "RyoikiHandTopologySummary layout must stay stable.");
    require(sizeof(RyoikiTwoHandRelation) == 40, "RyoikiTwoHandRelation layout must stay stable.");
    require(sizeof(RyoikiHandTopologySnapshot) == 344, "RyoikiHandTopologySnapshot layout must stay stable.");
}
}

int main()
{
    try
    {
        testAbiVersionIsCurrent();
        testGestureRecordingStatusLayout();
        testGestureDefinitionListLayout();
        testGestureRecordingPlaybackLayout();
        testGestureDtwDebugSnapshotLayout();
        testGestureRecognitionSnapshotIsTriviallyCopyablePod();
        testGestureRecognitionSnapshotStaysSmall();
        testGestureRecognitionSnapshotHeaderFieldsMatchOtherSnapshots();
        testGestureRecognitionSnapshotFieldOffsetsAreMonotonicAndNonOverlapping();
        testGestureRecognitionSnapshotReasonBuffersAreFixedAndNulTerminatable();
        testStructSizesUnchangedForExistingAbiStructs();
        std::cout << "RyoikiNative ABI layout tests passed. sizeof(RyoikiGestureRecognitionSnapshot)="
            << sizeof(RyoikiGestureRecognitionSnapshot) << " bytes.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
