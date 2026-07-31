#include "HandInput/Recognition/gesture_template_registry.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <sstream>
#include <utility>

namespace ryoiki::hand_input::recognition
{
struct GestureTemplateRegistry::Storage
{
    std::array<GestureTemplateRecord, kMaxGestureTemplates> templates{};
};

GestureTemplateRegistry::GestureTemplateRegistry()
    : storage_{std::make_unique<Storage>()}
{
}

GestureTemplateRegistry::~GestureTemplateRegistry() = default;

namespace
{
std::string formatFixed(const double value, const int precision)
{
    std::ostringstream stream;
    stream.precision(precision);
    stream << std::fixed << value;
    return stream.str();
}

// Ported GestureTemplateFactory.HasMeaningfulActionSignal.
bool hasMeaningfulActionSignal(const measurements::HandTopologySummary& topology) noexcept
{
    return topology.palmTravel >= kMinimumPalmTravel
        || topology.palmOrientationRangeRadians >= kMinimumPalmOrientationRangeRadians
        || topology.palmTurnScore >= kMinimumPalmTurnScore
        || topology.fingerStraightnessRangeMax >= kMinimumFingerStraightnessRangeForKind
        || topology.fingerStateTransitionCount > 0
        || topology.handScaleRatioRange >= kMinimumHandScaleRatioRange
        || topology.translationDistance >= kMinimumTranslationDistance;
}
}

bool isRecognizableGestureWindow(const measurements::GestureCandidateWindow& window) noexcept
{
    if (!window.valid || !window.topology.valid)
    {
        return false;
    }

    const auto& topology = window.topology;
    auto kind = measurements::detectGestureKind(window.motionScore);
    if (kind == measurements::GestureKind::Static && topology.palmTurnScore >= kMinimumPalmTurnScore)
    {
        kind = measurements::GestureKind::Dynamic;
    }
    if (kind == measurements::GestureKind::Static
        && (topology.fingerStateTransitionCount > 0
            || topology.fingerStraightnessRangeMax >= kMinimumFingerStraightnessRangeForKind
            || topology.handScaleRatioRange >= kMinimumHandScaleRatioRange
            || topology.translationDistance >= kMinimumTranslationDistance))
    {
        kind = measurements::GestureKind::Dynamic;
    }

    return kind == measurements::GestureKind::Dynamic && hasMeaningfulActionSignal(topology);
}

TemplateCreationResult createTemplateFromCandidateWindow(
    const measurements::GestureCandidateWindow& window) noexcept
{
    TemplateCreationResult result{};
    if (!window.valid)
    {
        result.rejectionReason = "No candidate window is available to register.";
        return result;
    }

    if (window.durationMs < kMinimumTemplateDurationMs)
    {
        result.rejectionReason = "Captured " + formatFixed(window.durationMs, 0)
            + " ms, but a template needs about 1.9 seconds (" + formatFixed(kMinimumTemplateDurationMs, 0)
            + "+ ms).";
        return result;
    }

    if (window.usableFrameCount < kMinimumTemplateUsableSampleCount)
    {
        result.rejectionReason = "Captured " + std::to_string(window.usableFrameCount)
            + " usable frame(s); need " + std::to_string(kMinimumTemplateUsableSampleCount)
            + " usable frames over about 1.9 seconds for a template.";
        return result;
    }

    const double usableFps = window.usableFrameCount / (window.durationMs / 1000.0);
    if (usableFps < kMinimumTemplateSampleRateFps)
    {
        result.rejectionReason = "Captured " + formatFixed(usableFps, 1)
            + " usable fps, but a template needs " + formatFixed(kMinimumTemplateSampleRateFps, 1)
            + "+ fps for reliable frame-by-frame recognition.";
        return result;
    }

    if (!isRecognizableGestureWindow(window))
    {
        const auto& topology = window.topology;
        result.rejectionReason = "Captured a stable pose, not a gesture movement. "
            "topology score=" + formatFixed(topology.topologyChangeScore, 3)
            + ", travel=" + formatFixed(topology.palmTravel, 3)
            + ", palm turn=" + formatFixed(topology.palmTurnScore, 3)
            + ", size range=" + formatFixed(topology.handScaleRatioRange, 3)
            + ", translation=" + formatFixed(topology.translationDistance, 3)
            + ", finger transitions=" + std::to_string(topology.fingerStateTransitionCount)
            + ". Move/flip/change shape more during the capture window.";
        return result;
    }

    result.created = true;
    result.sequence.frames = window.unifiedSequence;
    result.sequence.topology = window.topology;
    result.sequence.visualization = window.visualization;
    return result;
}

bool GestureTemplateRegistry::registerTemplate(
    const std::uint32_t templateId, UnifiedSequenceTemplate sequence) noexcept
{
    for (auto& slot : storage_->templates)
    {
        if (slot.active && slot.templateId == templateId)
        {
            slot.trials[0] = GestureTrial{GestureTrialKind::OneHandDynamic, std::move(sequence)};
            slot.trialCount = 1;
            return true;
        }
    }
    for (auto& slot : storage_->templates)
    {
        if (!slot.active)
        {
            slot.templateId = templateId;
            slot.trials[0] = GestureTrial{GestureTrialKind::OneHandDynamic, std::move(sequence)};
            slot.trialCount = 1;
            slot.active = true;
            return true;
        }
    }
    return false;
}

bool GestureTemplateRegistry::appendTemplateSet(
    const std::uint32_t templateId,
    const std::array<UnifiedSequenceTemplate, kRequiredGestureTemplateCount>& templates) noexcept
{
    GestureTemplateRecord* destination = nullptr;
    for (auto& slot : storage_->templates)
    {
        if (slot.active && slot.templateId == templateId)
        {
            destination = &slot;
            break;
        }
    }
    if (destination == nullptr)
    {
        for (auto& slot : storage_->templates)
        {
            if (!slot.active)
            {
                destination = &slot;
                break;
            }
        }
    }
    if (destination == nullptr
        || destination->trialCount + templates.size() > destination->trials.size())
    {
        return false;
    }
    if (!destination->active)
    {
        destination->templateId = templateId;
        destination->active = true;
    }
    for (const auto& sequence : templates)
    {
        destination->trials[destination->trialCount++] =
            GestureTrial{GestureTrialKind::OneHandDynamic, sequence};
    }
    return true;
}

bool GestureTemplateRegistry::appendTwoHandTemplateSet(
    const std::uint32_t templateId,
    const std::array<TwoHandTemplate, kRequiredGestureTemplateCount>& templates) noexcept
{
    GestureTemplateRecord* destination = nullptr;
    for (auto& slot : storage_->templates)
    {
        if (slot.active && slot.templateId == templateId) { destination = &slot; break; }
        if (destination == nullptr && !slot.active) destination = &slot;
    }
    if (destination == nullptr
        || destination->trialCount + templates.size() > destination->trials.size()) return false;
    if (!destination->active) { destination->templateId = templateId; destination->active = true; }
    for (const auto& value : templates)
        destination->trials[destination->trialCount++] =
            GestureTrial{GestureTrialKind::TwoHandDynamic, value};
    return true;
}

bool GestureTemplateRegistry::canAppendTemplateSet(const std::uint32_t templateId) const noexcept
{
    bool hasFreeSlot = false;
    for (const auto& slot : storage_->templates)
    {
        if (slot.active && slot.templateId == templateId)
            return slot.trialCount + kRequiredGestureTemplateCount <= slot.trials.size();
        hasFreeSlot = hasFreeSlot || !slot.active;
    }
    return hasFreeSlot;
}

bool GestureTemplateRegistry::restoreDefinition(
    const std::uint32_t templateId,
    const std::vector<UnifiedSequenceTemplate>& templates) noexcept
{
    if (templates.empty() || templates.size() > kMaxGestureTemplateTrialsPerDefinition) return false;
    GestureTemplateRecord* destination = nullptr;
    for (auto& slot : storage_->templates)
    {
        if (slot.active && slot.templateId == templateId) { destination = &slot; break; }
        if (destination == nullptr && !slot.active) destination = &slot;
    }
    if (destination == nullptr) return false;
    destination->active = true;
    destination->templateId = templateId;
    destination->trialCount = templates.size();
    for (std::size_t index = 0; index < templates.size(); ++index)
        destination->trials[index] =
            GestureTrial{GestureTrialKind::OneHandDynamic, templates[index]};
    return true;
}

bool GestureTemplateRegistry::restoreTrials(
    const std::uint32_t templateId, const std::vector<GestureTrial>& trials) noexcept
{
    if (trials.empty() || trials.size() > kMaxGestureTemplateTrialsPerDefinition) return false;
    GestureTemplateRecord* destination = nullptr;
    for (auto& slot : storage_->templates)
    {
        if (slot.active && slot.templateId == templateId) { destination = &slot; break; }
        if (destination == nullptr && !slot.active) destination = &slot;
    }
    if (destination == nullptr) return false;
    destination->active = true; destination->templateId = templateId;
    destination->trialCount = trials.size();
    std::copy(trials.begin(), trials.end(), destination->trials.begin());
    return true;
}

bool GestureTemplateRegistry::removeTemplate(const std::uint32_t templateId) noexcept
{
    for (auto& slot : storage_->templates)
    {
        if (slot.active && slot.templateId == templateId)
        {
            slot.active = false;
            slot.templateId = 0;
            slot.trialCount = 0;
            return true;
        }
    }
    return false;
}

void GestureTemplateRegistry::clear() noexcept
{
    for (auto& slot : storage_->templates)
    {
        slot.active = false;
        slot.templateId = 0;
        slot.trialCount = 0;
    }
}

std::size_t GestureTemplateRegistry::size() const noexcept
{
    return std::accumulate(storage_->templates.begin(), storage_->templates.end(), std::size_t{0},
        [](const std::size_t count, const auto& slot)
        {
            return count + (slot.active ? slot.trialCount : 0);
        });
}

const GestureTemplateRecord* GestureTemplateRegistry::find(const std::uint32_t templateId) const noexcept
{
    for (const auto& slot : storage_->templates)
    {
        if (slot.active && slot.templateId == templateId)
        {
            return &slot;
        }
    }
    return nullptr;
}

const std::array<GestureTemplateRecord, kMaxGestureTemplates>& GestureTemplateRegistry::records() const noexcept
{
    return storage_->templates;
}

BestMatchResult findBestMatch(
    const std::array<measurements::GestureCandidateWindow, measurements::kGestureCandidateDurationsMs.size()>&
        candidates,
    const std::size_t candidateCount,
    const GestureTemplateRegistry& registry) noexcept
{
    BestMatchResult result{};
    float bestScore = (std::numeric_limits<float>::max)();

    for (const auto& record : registry.records())
    {
        const auto oneHandCount = static_cast<std::size_t>(std::count_if(
            record.trials.begin(), record.trials.begin() + record.trialCount,
            [](const auto& trial) { return trial.kind == GestureTrialKind::OneHandDynamic; }));
        if (!record.active || oneHandCount < kRequiredGestureTemplateCount)
        {
            continue;
        }

        for (std::size_t trialIndex = 0; trialIndex < record.trialCount; ++trialIndex)
        {
            const auto* oneHand = std::get_if<UnifiedSequenceTemplate>(
                &record.trials[trialIndex].payload);
            if (oneHand == nullptr) continue;
            float templateBestScore = (std::numeric_limits<float>::max)();
            std::size_t templateBestCandidateIndex = candidateCount;
            for (std::size_t candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex)
            {
                const auto& candidate = candidates[candidateIndex];
                if (!candidate.valid) continue;

                auto outcome = compareCandidateToTemplate(candidate, *oneHand);
                const auto candidateDurationMs = static_cast<std::uint32_t>((std::min)(
                    static_cast<double>((std::numeric_limits<std::uint32_t>::max)()), candidate.durationMs));
                result.attempts.push_back(GestureTemplateAttempt{
                    record.templateId, trialIndex, candidateDurationMs, candidateIndex, outcome});

                if (!outcome.topologyRejectionReason.empty() || !outcome.comparison.eligible
                    || outcome.comparison.score >= templateBestScore) continue;
                templateBestScore = outcome.comparison.score;
                templateBestCandidateIndex = candidateIndex;
            }
            if (templateBestCandidateIndex == candidateCount || templateBestScore >= bestScore) continue;
            bestScore = templateBestScore;
            result.matched = true;
            result.templateId = record.templateId;
            result.templateTrialIndex = trialIndex;
            result.templateSequence = *oneHand;
            result.candidate = candidates[templateBestCandidateIndex];
            result.candidateDurationMs = static_cast<std::uint32_t>((std::min)(
                static_cast<double>((std::numeric_limits<std::uint32_t>::max)()), result.candidate.durationMs));
            result.comparison = compareCandidateToTemplate(
                result.candidate, *oneHand).comparison;
        }
    }

    return result;
}
}
