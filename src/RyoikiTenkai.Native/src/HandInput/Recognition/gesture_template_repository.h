#pragma once
#include "HandInput/Recognition/gesture_template_registry.h"
#include <memory>
#include <string>
#include <vector>
namespace ryoiki::hand_input::recognition {
inline constexpr std::size_t kMaxGestureRecordingFrames = 360;
struct GestureRecordingQuality {
 bool accepted{}; std::uint32_t sourceFrameCount{}; std::uint32_t validLandmarkFrameCount{};
 std::uint32_t highConfidenceFrameCount{}; float effectiveFps{}; float usableEffectiveFps{};
 std::string reason;
};
struct GestureRecordingFrame {
 double timeOffsetMs{}; std::uint32_t handCount{};
 std::array<float,2> confidence{}; std::array<float,2> handedness{};
 std::array<std::array<hand_perception::Landmark3f,measurements::kLandmarkCount>,2> normalizedSkeletons{};
};
struct GestureRecordingProvenance {
 std::string sourceId; std::uint32_t takeIndex{}; std::uint64_t capturedAtUs{};
 double durationMs{}; float averageConfidence{}; GestureRecordingQuality quality{};
 std::vector<GestureRecordingFrame> frames;
};
struct GestureDefinitionMetadata { std::uint32_t id{}; std::string name; bool enabled{true}; std::uint32_t takeCount{}; };
struct GestureBindingMetadata { std::uint32_t definitionId{}; bool enabled{true}; std::string actionType; std::string actionParameter; };
class GestureTemplateRepository final {
public:
 explicit GestureTemplateRepository(std::string databasePath); ~GestureTemplateRepository();
 GestureTemplateRepository(const GestureTemplateRepository&)=delete; GestureTemplateRepository& operator=(const GestureTemplateRepository&)=delete;
 bool open(std::string& error) noexcept;
 bool load(GestureTemplateRegistry&,std::vector<GestureDefinitionMetadata>&,std::string&) noexcept;
 bool appendThreeTakeSet(GestureTemplateRegistry&,std::uint32_t,const std::string&,const std::array<UnifiedSequenceTemplate,kRequiredGestureTemplateCount>&,std::string&) noexcept;
 bool appendThreeTakeSet(GestureTemplateRegistry&,std::uint32_t,const std::string&,const std::array<GestureTrial,kRequiredGestureTemplateCount>&,std::string&) noexcept;
 bool appendThreeTakeSet(GestureTemplateRegistry&,std::uint32_t,const std::string&,const std::array<GestureTrial,kRequiredGestureTemplateCount>&,const std::array<GestureRecordingProvenance,kRequiredGestureTemplateCount>&,std::string&) noexcept;
 bool listRecordings(std::uint32_t,std::vector<GestureRecordingProvenance>&,std::string&) noexcept;
 bool exportRecording(std::uint32_t,std::uint32_t,const std::string&,std::string&) noexcept;
 bool list(std::vector<GestureDefinitionMetadata>&,std::string&) noexcept;
 bool updateMetadata(std::uint32_t,const std::string&,bool,std::string&) noexcept;
 bool remove(std::uint32_t,GestureTemplateRegistry&,std::string&) noexcept;
 bool listBindings(std::vector<GestureBindingMetadata>&,std::string&) noexcept;
 bool upsertBinding(const GestureBindingMetadata&,std::string&) noexcept;
 bool removeBinding(std::uint32_t,std::string&) noexcept;
private: struct Impl; std::unique_ptr<Impl> impl_; };
}
