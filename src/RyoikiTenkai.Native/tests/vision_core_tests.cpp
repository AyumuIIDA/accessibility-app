#include "Buffers/frame_pool.h"
#include "Buffers/tensor_buffer.h"
#include "Geometry/hand_geometry_processor.h"
#include "Pipeline/latest_frame_slot.h"
#include "Pipeline/perception_mailbox.h"
#include "Rendering/latest_render_packet_slot.h"
#include "Runtime/camera_device_selection.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
using namespace std::chrono_literals;

void require(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error{message};
    }
}

void requireNear(const float actual, const float expected, const float tolerance, const char* message)
{
    require(std::isfinite(actual), "Expected a finite value.");
    require(std::abs(actual - expected) <= tolerance, message);
}

std::shared_ptr<ryoiki::buffers::FrameBuffer> createFrame(
    const std::uint64_t frameId,
    const std::uint32_t width = 4,
    const std::uint32_t height = 2,
    const std::uint8_t blue = 10,
    const std::uint8_t green = 20,
    const std::uint8_t red = 30)
{
    auto frame = std::make_shared<ryoiki::buffers::FrameBuffer>();
    require(frame->prepare(width, height, frameId, frameId * 100), "Frame preparation failed.");
    auto& pixels = frame->writablePixels();
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
    {
        pixels[offset] = blue;
        pixels[offset + 1] = green;
        pixels[offset + 2] = red;
        pixels[offset + 3] = 255;
    }
    return frame;
}

std::size_t tensorOffset(const int x, const int y, const int width)
{
    return static_cast<std::size_t>((y * width + x) * 3);
}

void requireRgb(
    const ryoiki::buffers::FloatTensorBuffer& tensor,
    const int x,
    const int y,
    const int width,
    const float red,
    const float green,
    const float blue,
    const char* message)
{
    constexpr float kTolerance = 0.001F;
    const auto offset = tensorOffset(x, y, width);
    requireNear(tensor.data()[offset], red, kTolerance, message);
    requireNear(tensor.data()[offset + 1], green, kTolerance, message);
    requireNear(tensor.data()[offset + 2], blue, kTolerance, message);
}

void testFramePool()
{
    ryoiki::buffers::FramePool pool{2};
    require(pool.capacity() == 2, "Pool reported an incorrect capacity.");

    auto first = pool.tryAcquire();
    auto second = pool.tryAcquire();
    require(first != nullptr && second != nullptr, "Pool did not provide its capacity.");
    require(first.get() != second.get(), "Pool returned the same frame twice concurrently.");
    require(pool.tryAcquire() == nullptr, "Exhausted pool unexpectedly returned a frame.");
    require(pool.tryAcquire() == nullptr, "Exhausted pool unexpectedly returned a second frame.");
    require(pool.droppedAcquisitions() == 2, "Pool did not count dropped acquisitions.");

    const auto* releasedAddress = first.get();
    first.reset();
    auto reused = pool.tryAcquire();
    require(reused != nullptr, "Released frame did not return to the pool.");
    require(reused.get() == releasedAddress, "Pool did not reuse the released frame.");

    pool.resetStatistics();
    require(pool.droppedAcquisitions() == 0, "Pool statistics were not reset.");
}

void testLatestFrameDelivery()
{
    const auto first = createFrame(1);
    const auto second = createFrame(2);

    ryoiki::pipeline::LatestFrameSlot displaySlot;
    displaySlot.publish(first);
    displaySlot.publish(second);
    require(displaySlot.snapshot()->frameId() == 2, "Display slot did not retain the latest frame.");

    ryoiki::pipeline::PerceptionMailbox mailbox;
    mailbox.reset();
    mailbox.publish(first);
    mailbox.publish(second);
    require(mailbox.droppedFrames() == 1, "Mailbox did not count the overwritten frame.");
    require(mailbox.waitForLatest()->frameId() == 2, "Mailbox did not return the latest frame.");
    mailbox.stop();
    require(mailbox.waitForLatest() == nullptr, "Stopped mailbox returned a frame.");
}

void testMailboxProducerConsumer()
{
    ryoiki::pipeline::PerceptionMailbox mailbox;
    mailbox.reset();

    auto consumer = std::async(std::launch::async, [&mailbox]
    {
        return mailbox.waitForLatest();
    });
    mailbox.publish(createFrame(17));
    require(consumer.wait_for(2s) == std::future_status::ready, "Mailbox consumer did not wake for a frame.");
    const auto delivered = consumer.get();
    require(delivered != nullptr && delivered->frameId() == 17, "Mailbox consumer received the wrong frame.");

    auto stoppedConsumer = std::async(std::launch::async, [&mailbox]
    {
        return mailbox.waitForLatest();
    });
    mailbox.stop();
    require(stoppedConsumer.wait_for(2s) == std::future_status::ready, "Mailbox stop did not release a waiter.");
    require(stoppedConsumer.get() == nullptr, "Mailbox stop released a waiter with a frame.");

    mailbox.publish(createFrame(18));
    require(mailbox.waitForLatest() == nullptr, "Stopped mailbox accepted a published frame.");
}

void testPalmLetterboxAndRgbPacking()
{
    ryoiki::geometry::HandGeometryProcessor processor;
    ryoiki::buffers::FloatTensorBuffer tensor{{1, 192, 192, 3}};
    ryoiki::geometry::PalmPreprocessResult result{};

    const auto wideFrame = createFrame(3, 4, 2, 11, 67, 203);
    const auto originalPixels = wideFrame->pixels();
    require(processor.preprocessPalm(*wideFrame, tensor, result), "Wide palm preprocessing failed.");
    requireRgb(tensor, 96, 96, 192, 203.0F / 255.0F, 67.0F / 255.0F, 11.0F / 255.0F,
        "BGRA to RGB tensor packing is incorrect.");
    requireRgb(tensor, 96, 0, 192, 0.0F, 0.0F, 0.0F, "Wide-image top padding is not zero.");
    requireNear(result.transform.padLeft, 0.0F, 0.0001F, "Wide-image horizontal padding is incorrect.");
    requireNear(result.transform.padTop, 48.0F, 0.0001F, "Wide-image vertical padding is incorrect.");
    require(wideFrame->pixels() == originalPixels, "Palm preprocessing mutated the source frame.");

    const ryoiki::geometry::Point2f widePoint{1.25F, 0.75F};
    const auto wideRoundTrip = result.transform.tensorToSource(result.transform.sourceToTensor(widePoint));
    requireNear(wideRoundTrip.x, widePoint.x, 0.0001F, "Wide-image X transform is not invertible.");
    requireNear(wideRoundTrip.y, widePoint.y, 0.0001F, "Wide-image Y transform is not invertible.");

    const auto tallFrame = createFrame(4, 2, 4, 7, 101, 241);
    require(processor.preprocessPalm(*tallFrame, tensor, result), "Tall palm preprocessing failed.");
    requireRgb(tensor, 96, 96, 192, 241.0F / 255.0F, 101.0F / 255.0F, 7.0F / 255.0F,
        "Tall-image RGB tensor packing is incorrect.");
    requireRgb(tensor, 0, 96, 192, 0.0F, 0.0F, 0.0F, "Tall-image left padding is not zero.");
    requireNear(result.transform.padLeft, 48.0F, 0.0001F, "Tall-image horizontal padding is incorrect.");
    requireNear(result.transform.padTop, 0.0F, 0.0001F, "Tall-image vertical padding is incorrect.");
}

void requireRegionMapping(
    ryoiki::geometry::HandGeometryProcessor& processor,
    const ryoiki::buffers::FrameBuffer& frame,
    const ryoiki::geometry::RotatedRegion& region,
    const ryoiki::geometry::Point2f expectedTopLeft,
    const ryoiki::geometry::Point2f expectedTopRight,
    const ryoiki::geometry::Point2f expectedBottomLeft)
{
    constexpr float kTolerance = 0.0002F;
    ryoiki::buffers::FloatTensorBuffer tensor{{1, 224, 224, 3}};
    ryoiki::geometry::HandPreprocessResult result{};
    require(processor.preprocessHand(frame, region, tensor, result), "Hand ROI preprocessing failed.");

    const auto topLeft = result.tensorToSource.transform({0.0F, 0.0F});
    const auto topRight = result.tensorToSource.transform({224.0F, 0.0F});
    const auto bottomLeft = result.tensorToSource.transform({0.0F, 224.0F});
    requireNear(topLeft.x, expectedTopLeft.x, kTolerance, "ROI top-left X mapping is incorrect.");
    requireNear(topLeft.y, expectedTopLeft.y, kTolerance, "ROI top-left Y mapping is incorrect.");
    requireNear(topRight.x, expectedTopRight.x, kTolerance, "ROI top-right X mapping is incorrect.");
    requireNear(topRight.y, expectedTopRight.y, kTolerance, "ROI top-right Y mapping is incorrect.");
    requireNear(bottomLeft.x, expectedBottomLeft.x, kTolerance, "ROI bottom-left X mapping is incorrect.");
    requireNear(bottomLeft.y, expectedBottomLeft.y, kTolerance, "ROI bottom-left Y mapping is incorrect.");

    const auto mappedCenter = result.sourceToTensor.transform(region.center);
    const auto restoredCenter = result.tensorToSource.transform(mappedCenter);
    requireNear(restoredCenter.x, region.center.x, kTolerance, "ROI X transform is not invertible.");
    requireNear(restoredCenter.y, region.center.y, kTolerance, "ROI Y transform is not invertible.");
}

void testRotatedHandRegions()
{
    constexpr float kHalfPi = 1.57079632679489661923F;
    const auto frame = createFrame(5, 8, 8, 10, 20, 30);
    ryoiki::geometry::HandGeometryProcessor processor;

    requireRegionMapping(processor, *frame, {{4.0F, 4.0F}, 4.0F, 2.0F, 0.0F},
        {2.0F, 3.0F}, {6.0F, 3.0F}, {2.0F, 5.0F});
    requireRegionMapping(processor, *frame, {{4.0F, 4.0F}, 4.0F, 2.0F, kHalfPi},
        {5.0F, 2.0F}, {5.0F, 6.0F}, {3.0F, 2.0F});
    requireRegionMapping(processor, *frame, {{4.0F, 4.0F}, 4.0F, 2.0F, -kHalfPi},
        {3.0F, 6.0F}, {3.0F, 2.0F}, {5.0F, 6.0F});

    ryoiki::buffers::FloatTensorBuffer paddedTensor{{1, 224, 224, 3}};
    ryoiki::geometry::HandPreprocessResult paddedResult{};
    require(processor.preprocessHand(*frame, {{0.0F, 0.0F}, 4.0F, 4.0F, 0.0F}, paddedTensor, paddedResult),
        "Out-of-bounds hand ROI preprocessing failed.");
    requireRgb(paddedTensor, 0, 0, 224, 0.0F, 0.0F, 0.0F, "Out-of-bounds ROI padding is not zero.");
}

void testInvalidGeometryInputs()
{
    ryoiki::geometry::HandGeometryProcessor processor;
    const auto frame = createFrame(6);
    ryoiki::geometry::PalmPreprocessResult palmResult{};
    ryoiki::geometry::HandPreprocessResult handResult{};
    ryoiki::buffers::FloatTensorBuffer wrongPalmShape{{1, 191, 192, 3}};
    ryoiki::buffers::FloatTensorBuffer wrongHandShape{{1, 224, 224, 4}};
    ryoiki::buffers::FloatTensorBuffer handTensor{{1, 224, 224, 3}};

    require(!processor.preprocessPalm(*frame, wrongPalmShape, palmResult),
        "Palm preprocessing accepted an incorrect tensor shape.");
    require(!processor.preprocessHand(*frame, {{1.5F, 0.5F}, 4.0F, 2.0F, 0.0F}, wrongHandShape, handResult),
        "Hand preprocessing accepted an incorrect tensor shape.");
    require(!processor.preprocessHand(*frame, {{1.5F, 0.5F}, 0.0F, 2.0F, 0.0F}, handTensor, handResult),
        "Hand preprocessing accepted a zero-width ROI.");
    require(!processor.preprocessHand(*frame, {{1.5F, 0.5F}, 4.0F, 0.0F, 0.0F}, handTensor, handResult),
        "Hand preprocessing accepted a zero-height ROI.");
    require(!processor.preprocessHand(*frame, {{1.5F, 0.5F}, -1.0F, 2.0F, 0.0F}, handTensor, handResult),
        "Hand preprocessing accepted a negative-width ROI.");

    ryoiki::buffers::FrameBuffer emptyFrame;
    ryoiki::buffers::FloatTensorBuffer palmTensor{{1, 192, 192, 3}};
    require(!processor.preprocessPalm(emptyFrame, palmTensor, palmResult),
        "Palm preprocessing accepted an empty frame.");

    bool zeroShapeRejected = false;
    try
    {
        [[maybe_unused]] ryoiki::buffers::FloatTensorBuffer zeroShape{{1, 0, 192, 3}};
    }
    catch (const std::invalid_argument&)
    {
        zeroShapeRejected = true;
    }
    require(zeroShapeRejected, "Tensor buffer accepted a zero dimension.");
}

void testUserFacingCameraSelection()
{
    require(ryoiki::runtime::selectUserFacingCamera(
        std::vector<std::wstring>{L"Surface Camera Rear", L"Surface Camera Front"}) == 1,
        "Camera selection did not prefer the user-facing Surface camera.");
    require(ryoiki::runtime::selectUserFacingCamera(
        std::vector<std::wstring>{L"Surface IR Camera Front", L"Surface Camera Front"}) == 1,
        "Camera selection preferred an infrared camera over the front color camera.");
    require(ryoiki::runtime::selectUserFacingCamera(
        std::vector<std::wstring>{L"REAR CAMERA", L"FRONT CAMERA"}) == 1,
        "Camera selection was case-sensitive.");
    require(ryoiki::runtime::selectUserFacingCamera(
        std::vector<std::wstring>{L"USB Camera", L"External Capture"}) == 0,
        "Camera selection did not preserve enumeration-order fallback.");
}

void testLatestRenderPacketKeepsFrameAndMetadataTogether()
{
    ryoiki::rendering::LatestRenderPacketSlot slot;
    ryoiki::hand_perception::HandPerceptionResult firstResult;
    firstResult.hand.detected = true;
    firstResult.hand.confidence = 0.75F;
    slot.publish({createFrame(11), firstResult});

    const auto first = slot.snapshot();
    require(first.has_value() && first->frame != nullptr && first->frame->frameId() == 11,
        "Render packet slot did not retain the source frame.");
    require(first->perception.hand.detected
            && first->perception.hand.confidence == 0.75F,
        "Render packet slot detached metadata from its source frame.");

    ryoiki::hand_perception::HandPerceptionResult secondResult;
    slot.publish({createFrame(12), secondResult});
    const auto latest = slot.snapshot();
    require(latest.has_value() && latest->frame->frameId() == 12,
        "Render packet slot did not replace a stale packet.");
    require(!latest->perception.hand.detected,
        "Render packet slot mixed metadata from different frames.");

    slot.clear();
    require(!slot.snapshot().has_value(), "Render packet slot clear retained a packet.");
}
}

int main()
{
    try
    {
        testFramePool();
        testLatestFrameDelivery();
        testMailboxProducerConsumer();
        testPalmLetterboxAndRgbPacking();
        testRotatedHandRegions();
        testInvalidGeometryInputs();
        testUserFacingCameraSelection();
        testLatestRenderPacketKeepsFrameAndMetadataTogether();
        std::cout << "VisionCore tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
