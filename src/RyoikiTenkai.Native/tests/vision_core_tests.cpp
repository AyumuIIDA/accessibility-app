#include "Buffers/frame_pool.h"
#include "Buffers/tensor_buffer.h"
#include "Geometry/hand_geometry_processor.h"
#include "Geometry/d3d11_hand_geometry_processor.h"
#include "Pipeline/latest_frame_slot.h"
#include "Pipeline/perception_mailbox.h"
#include "Rendering/latest_render_packet_slot.h"
#include "Rendering/frame_transforms.h"
#include "Rendering/hand_3d_plot.h"
#include "Runtime/camera_device_selection.h"
#include "Runtime/frame_orientation.h"
#include "Runtime/d3d11_device.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
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
    const std::uint8_t red = 30,
    const ryoiki::runtime::FrameRotation orientation =
        ryoiki::runtime::FrameRotation::None)
{
    auto frame = std::make_shared<ryoiki::buffers::FrameBuffer>();
    require(frame->prepare(width, height, frameId, frameId * 100, orientation),
        "Frame preparation failed.");
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

void testGpuPreprocessMatchesCpuReference()
{
    std::string error;
    auto device = ryoiki::runtime::D3d11Device::create(error);
    require(device != nullptr, "D3D11 test device creation failed.");
    auto gpuProcessor = ryoiki::geometry::D3d11HandGeometryProcessor::create(device, error);
    require(gpuProcessor != nullptr, "GPU geometry processor creation failed.");

    auto frame = createFrame(40, 64, 48, 0, 0, 0);
    auto& pixels = frame->writablePixels();
    for (std::uint32_t y = 0; y < frame->height(); ++y)
    {
        for (std::uint32_t x = 0; x < frame->width(); ++x)
        {
            const auto offset = static_cast<std::size_t>((y * frame->width() + x) * 4U);
            pixels[offset] = static_cast<std::uint8_t>((x * 3U + y) & 0xffU);
            pixels[offset + 1] = static_cast<std::uint8_t>((x + y * 5U) & 0xffU);
            pixels[offset + 2] = static_cast<std::uint8_t>((x * 2U + y * 3U) & 0xffU);
            pixels[offset + 3] = 255;
        }
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = frame->width();
    description.Height = frame->height();
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = pixels.data();
    initialData.SysMemPitch = frame->stride();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    require(SUCCEEDED(device->device()->CreateTexture2D(
        &description, &initialData, &texture)), "GPU test texture creation failed.");
    require(frame->attachGpuResource(texture, 0, {}), "GPU test texture attachment failed.");

    ryoiki::geometry::HandGeometryProcessor cpuProcessor;
    ryoiki::buffers::FloatTensorBuffer cpuPalm{{1, 192, 192, 3}};
    ryoiki::buffers::FloatTensorBuffer gpuPalm{{1, 192, 192, 3}};
    ryoiki::geometry::PalmPreprocessResult cpuPalmResult{};
    ryoiki::geometry::PalmPreprocessResult gpuPalmResult{};
    require(cpuProcessor.preprocessPalm(*frame, cpuPalm, cpuPalmResult),
        "CPU palm reference preprocessing failed.");
    require(gpuProcessor->preprocessPalm(*frame, gpuPalm, gpuPalmResult),
        "GPU palm preprocessing failed.");

    float maximumDifference = 0.0F;
    double totalDifference = 0.0;
    for (std::size_t index = 0; index < cpuPalm.elementCount(); ++index)
    {
        const float difference = std::abs(cpuPalm.data()[index] - gpuPalm.data()[index]);
        maximumDifference = (std::max)(maximumDifference, difference);
        totalDifference += difference;
    }
    const double meanDifference = totalDifference / cpuPalm.elementCount();
    require(maximumDifference < 0.025F, "GPU palm tensor maximum error is too large.");
    require(meanDifference < 0.001F, "GPU palm tensor mean error is too large.");

    const ryoiki::geometry::RotatedRegion region{{31.5F, 23.5F}, 38.0F, 30.0F, 0.27F};
    ryoiki::buffers::FloatTensorBuffer cpuHand{{1, 224, 224, 3}};
    ryoiki::buffers::FloatTensorBuffer gpuHand{{1, 224, 224, 3}};
    ryoiki::geometry::HandPreprocessResult cpuHandResult{};
    ryoiki::geometry::HandPreprocessResult gpuHandResult{};
    require(cpuProcessor.preprocessHand(*frame, region, cpuHand, cpuHandResult),
        "CPU hand reference preprocessing failed.");
    require(gpuProcessor->preprocessHand(*frame, region, gpuHand, gpuHandResult),
        "GPU hand preprocessing failed.");
    maximumDifference = 0.0F;
    totalDifference = 0.0;
    for (std::size_t index = 0; index < cpuHand.elementCount(); ++index)
    {
        const float difference = std::abs(cpuHand.data()[index] - gpuHand.data()[index]);
        maximumDifference = (std::max)(maximumDifference, difference);
        totalDifference += difference;
    }
    require(maximumDifference < 0.025F, "GPU hand tensor maximum error is too large.");
    require(totalDifference / cpuHand.elementCount() < 0.001F,
        "GPU hand tensor mean error is too large.");
}

void testPalmSamplingUsesSharedPixelCenterOrientation()
{
    using ryoiki::geometry::Point2f;
    using ryoiki::runtime::FrameRotation;

    constexpr std::uint32_t kWidth = 192;
    constexpr std::uint32_t kHeight = 96;
    auto frame = createFrame(
        31, kWidth, kHeight, 0, 0, 0, FrameRotation::Clockwise90);
    auto& pixels = frame->writablePixels();
    const auto setMarker = [&pixels](
        const int x,
        const int y,
        const std::uint8_t blue,
        const std::uint8_t green,
        const std::uint8_t red)
    {
        const auto offset = static_cast<std::size_t>((y * kWidth + x) * 4);
        pixels[offset] = blue;
        pixels[offset + 1] = green;
        pixels[offset + 2] = red;
        pixels[offset + 3] = 255;
    };
    setMarker(0, 0, 11, 21, 31);
    setMarker(191, 0, 41, 51, 61);
    setMarker(0, 95, 71, 81, 91);
    setMarker(191, 95, 101, 111, 121);

    ryoiki::geometry::HandGeometryProcessor processor;
    ryoiki::buffers::FloatTensorBuffer tensor{{1, 192, 192, 3}};
    ryoiki::geometry::PalmPreprocessResult result{};
    require(processor.preprocessPalm(*frame, tensor, result),
        "Oriented marker preprocessing failed.");

    auto storageToUprightCenters = ryoiki::geometry::toPixelCenterTransform(
        ryoiki::geometry::createStorageToUprightTransform(
            kWidth, kHeight, FrameRotation::Clockwise90));
    ryoiki::geometry::AffineTransform uprightToStorageCenters{};
    require(ryoiki::geometry::invert(
            storageToUprightCenters, uprightToStorageCenters),
        "Pixel-center orientation transform was not invertible.");

    struct Marker
    {
        Point2f storage;
        float red;
        float green;
        float blue;
    };
    constexpr std::array<Marker, 4> kMarkers{{
        {{0.0F, 0.0F}, 31.0F, 21.0F, 11.0F},
        {{191.0F, 0.0F}, 61.0F, 51.0F, 41.0F},
        {{0.0F, 95.0F}, 91.0F, 81.0F, 71.0F},
        {{191.0F, 95.0F}, 121.0F, 111.0F, 101.0F}}};

    for (const auto& marker : kMarkers)
    {
        const auto upright = storageToUprightCenters.transform(marker.storage);
        const auto tensorPoint = result.transform.sourceToTensor(upright);
        requireNear(tensorPoint.x, std::round(tensorPoint.x), 0.0001F,
            "Marker did not map to an exact tensor pixel X.");
        requireNear(tensorPoint.y, std::round(tensorPoint.y), 0.0001F,
            "Marker did not map to an exact tensor pixel Y.");
        requireRgb(
            tensor,
            static_cast<int>(std::round(tensorPoint.x)),
            static_cast<int>(std::round(tensorPoint.y)),
            192,
            marker.red / 255.0F,
            marker.green / 255.0F,
            marker.blue / 255.0F,
            "Palm sampling did not preserve the oriented corner marker.");

        const auto restoredUpright = result.transform.tensorToSource(tensorPoint);
        const auto restoredStorage = uprightToStorageCenters.transform(restoredUpright);
        requireNear(restoredStorage.x, marker.storage.x, 0.0001F,
            "Palm tensor round trip did not restore storage pixel-center X.");
        requireNear(restoredStorage.y, marker.storage.y, 0.0001F,
            "Palm tensor round trip did not restore storage pixel-center Y.");
    }
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

std::vector<std::uint8_t> createOrientationSource(const bool bottomUp)
{
    constexpr std::size_t kStride = 12;
    std::vector<std::uint8_t> pixels(kStride * 3, 0);
    const std::uint8_t logicalRows[3][2]{{1, 2}, {3, 4}, {5, 6}};
    for (std::size_t logicalY = 0; logicalY < 3; ++logicalY)
    {
        const std::size_t memoryY = bottomUp ? 2 - logicalY : logicalY;
        for (std::size_t x = 0; x < 2; ++x)
        {
            const std::size_t offset = memoryY * kStride + x * 4;
            pixels[offset] = logicalRows[logicalY][x];
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}

void requireOrientedBlueValues(
    const std::vector<std::uint8_t>& pixels,
    const std::vector<std::uint8_t>& expected,
    const char* message)
{
    require(pixels.size() == expected.size() * 4, message);
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        require(pixels[index * 4] == expected[index], message);
    }
}

void testFrameOrientationNormalization()
{
    using ryoiki::runtime::FrameRotation;

    std::vector<std::uint8_t> destination;
    const auto topDown = createOrientationSource(false);
    require(ryoiki::runtime::copyStorageBgra32(
            topDown.data(), 12, 2, 3, destination),
        "Top-down storage copy failed.");
    requireOrientedBlueValues(destination, {1, 2, 3, 4, 5, 6},
        "Positive stride changed top-down row order.");

    const auto bottomUp = createOrientationSource(true);
    require(ryoiki::runtime::copyStorageBgra32(
            bottomUp.data() + 24, -12, 2, 3, destination),
        "Bottom-up storage copy failed.");
    requireOrientedBlueValues(destination, {1, 2, 3, 4, 5, 6},
        "Negative stride did not restore top-down row order.");

    require(!ryoiki::runtime::copyStorageBgra32(
            topDown.data(), 4, 2, 3, destination),
        "Storage copy accepted a stride shorter than one row.");

    require(ryoiki::runtime::orientedSize(2, 3, FrameRotation::Clockwise90).width == 3,
        "90-degree orientation did not swap width.");
    require(ryoiki::runtime::orientedSize(2, 3, FrameRotation::Clockwise270).height == 2,
        "270-degree orientation did not swap height.");
    require(ryoiki::runtime::orientedSize(2, 3, FrameRotation::Clockwise180).width == 2,
        "180-degree orientation changed width.");
}

void testFrameOrientationRemainsMetadataUntilSampling()
{
    using ryoiki::runtime::FrameRotation;

    const auto source = createOrientationSource(false);
    std::vector<std::uint8_t> storagePixels;
    require(ryoiki::runtime::copyStorageBgra32(
            source.data(), 12, 2, 3, storagePixels),
        "Storage-order frame copy failed.");
    requireOrientedBlueValues(storagePixels, {1, 2, 3, 4, 5, 6},
        "Capture copy physically rotated storage pixels.");

    ryoiki::buffers::FrameBuffer frame;
    require(frame.prepare(2, 3, 19, 1900, FrameRotation::Clockwise90),
        "Oriented frame preparation failed.");
    frame.writablePixels() = storagePixels;
    require(frame.width() == 2 && frame.height() == 3,
        "Storage dimensions were replaced by upright dimensions.");
    require(frame.uprightWidth() == 3 && frame.uprightHeight() == 2,
        "Upright dimensions did not apply orientation metadata.");

    ryoiki::geometry::HandGeometryProcessor processor;
    ryoiki::buffers::FloatTensorBuffer tensor{{1, 192, 192, 3}};
    ryoiki::geometry::PalmPreprocessResult result{};
    require(processor.preprocessPalm(frame, tensor, result),
        "Palm preprocessing did not consume orientation metadata.");
    requireNear(result.transform.scaleX, 64.0F, 0.0001F,
        "Palm X scale did not use upright width.");
    requireNear(result.transform.scaleY, 64.0F, 0.0001F,
        "Palm Y scale did not use upright height.");
    requireNear(result.transform.padLeft, 0.0F, 0.0001F,
        "Palm orientation introduced incorrect horizontal padding.");
    requireNear(result.transform.padTop, 32.0F, 0.0001F,
        "Palm orientation introduced incorrect vertical padding.");
}

void testFrameTransformsShareOneViewportContract()
{
    using ryoiki::rendering::FrameTransforms;
    using ryoiki::geometry::Point2f;
    using ryoiki::runtime::FrameRotation;

    FrameTransforms transforms{};
    require(ryoiki::rendering::createFrameTransforms(
            2, 3, FrameRotation::Clockwise90, 8, 8, true, transforms),
        "Frame transform creation failed.");
    requireNear(transforms.contentLeft, 0.0F, 0.0001F,
        "Letterbox horizontal offset was incorrect.");
    requireNear(transforms.contentTop, 4.0F / 3.0F, 0.0001F,
        "Letterbox vertical offset was incorrect.");

    const Point2f storageTopLeft{0.0F, 0.0F};
    const auto uprightTopRight = transforms.storageToUpright.transform(storageTopLeft);
    requireNear(uprightTopRight.x, 3.0F, 0.0001F,
        "Storage rotation did not map the top-left boundary to upright top-right.");
    requireNear(uprightTopRight.y, 0.0F, 0.0001F,
        "Storage rotation changed the expected upright Y coordinate.");

    const auto viewportPoint = transforms.storageToViewport.transform(storageTopLeft);
    const auto composedPoint = transforms.uprightToViewport.transform(uprightTopRight);
    requireNear(viewportPoint.x, composedPoint.x, 0.0001F,
        "Storage and metadata transforms did not share the same viewport mapping.");
    requireNear(viewportPoint.y, composedPoint.y, 0.0001F,
        "Storage and metadata transforms disagreed on viewport Y.");

    const auto uprightRoundTrip = transforms.viewportToUpright.transform(composedPoint);
    requireNear(uprightRoundTrip.x, uprightTopRight.x, 0.0001F,
        "Viewport inverse did not restore upright X.");
    requireNear(uprightRoundTrip.y, uprightTopRight.y, 0.0001F,
        "Viewport inverse did not restore upright Y.");

    FrameTransforms oddDimensions{};
    require(ryoiki::rendering::createFrameTransforms(
            641, 479, FrameRotation::Clockwise270, 1001, 701, false, oddDimensions),
        "Odd-dimension frame transform creation failed.");
    const Point2f nonSymmetric{137.25F, 288.75F};
    const auto upright = oddDimensions.storageToUpright.transform(nonSymmetric);
    const auto storageRoundTrip = oddDimensions.uprightToStorage.transform(upright);
    requireNear(storageRoundTrip.x, nonSymmetric.x, 0.0001F,
        "Orientation inverse did not restore odd-dimension X.");
    requireNear(storageRoundTrip.y, nonSymmetric.y, 0.0001F,
        "Orientation inverse did not restore odd-dimension Y.");
}

void testSplitViewportAndHand3dProjection()
{
    using ryoiki::rendering::FrameTransforms;
    using ryoiki::rendering::Hand3dView;
    using ryoiki::rendering::PlotViewport;
    using ryoiki::rendering::ViewportRect;
    using ryoiki::runtime::FrameRotation;

    FrameTransforms transforms{};
    require(ryoiki::rendering::createFrameTransforms(
            640, 480, FrameRotation::None, ViewportRect{20, 10, 700, 500},
            false, transforms),
        "Split camera viewport transform creation failed.");
    requireNear(transforms.contentLeft, 36.6667F, 0.001F,
        "Camera letterbox did not include the split viewport X offset.");
    requireNear(transforms.contentTop, 10.0F, 0.0001F,
        "Camera letterbox did not include the split viewport Y offset.");

    ryoiki::hand_perception::HandLandmarkResult hand{};
    hand.detected = true;
    hand.worldLandmarks[0] = {1.0F, 2.0F, 3.0F};
    for (std::size_t index = 1; index < hand.worldLandmarks.size(); ++index)
    {
        hand.worldLandmarks[index] = hand.worldLandmarks[0];
    }
    hand.worldLandmarks[8].x += 0.06F;

    const PlotViewport plot{720.0F, 10.0F, 1000.0F, 510.0F};
    const Hand3dView frontView{
        0.0F,
        0.0F,
        0.12F,
        ryoiki::presentation::HandPresentationMode::MirrorDirect};
    const auto projection = ryoiki::rendering::projectHand3d(hand, plot, frontView);
    require(projection.valid, "Valid world landmarks did not produce a 3D projection.");
    requireNear(projection.points[0].position.x, 860.0F, 0.0001F,
        "The wrist was not centered in the 3D viewport.");
    requireNear(projection.points[0].position.y, 260.0F, 0.0001F,
        "The wrist was not vertically centered in the 3D viewport.");
    require(projection.points[8].position.x < projection.points[0].position.x,
        "Front-camera mirroring was not applied to positive hand X.");
    const Hand3dView physicalView{
        0.0F,
        0.0F,
        0.12F,
        ryoiki::presentation::HandPresentationMode::Physical};
    const auto physicalProjection =
        ryoiki::rendering::projectHand3d(hand, plot, physicalView);
    require(physicalProjection.points[8].position.x
            > physicalProjection.points[0].position.x,
        "Physical presentation did not preserve positive hand X.");

    hand.worldLandmarks[8] = hand.worldLandmarks[0];
    hand.worldLandmarks[8].y += 0.06F;
    const auto downwardProjection = ryoiki::rendering::projectHand3d(hand, plot, frontView);
    require(downwardProjection.points[8].position.y > downwardProjection.points[0].position.y,
        "Image-space positive Y did not remain visually downward in the 3D plot.");

    ryoiki::hand_perception::HandLandmarkResult orientedHand{};
    orientedHand.detected = true;
    orientedHand.worldLandmarks[0] = {0.0F, 0.0F, 0.0F};
    orientedHand.worldLandmarks[5] = {0.04F, -0.05F, 0.0F};
    orientedHand.worldLandmarks[9] = {0.0F, -0.07F, 0.0F};
    orientedHand.worldLandmarks[13] = {-0.02F, -0.06F, 0.0F};
    orientedHand.worldLandmarks[17] = {-0.04F, -0.05F, 0.0F};
    const auto palmNormal = ryoiki::rendering::calculatePalmNormal(orientedHand);
    requireNear(palmNormal.x, 0.0F, 0.0001F,
        "Palm normal X did not match the displayed direction basis.");
    requireNear(palmNormal.y, 0.0F, 0.0001F,
        "Palm normal Y did not match the displayed direction basis.");
    requireNear(palmNormal.z, -1.0F, 0.0001F,
        "Palm normal Z did not match the displayed direction basis.");
    const Hand3dView sideView{
        1.5707963F,
        0.0F,
        0.12F,
        ryoiki::presentation::HandPresentationMode::MirrorDirect};
    const auto orientedProjection = ryoiki::rendering::projectHand3d(
        orientedHand, plot, sideView);
    require(orientedProjection.hasPalmDirection,
        "A valid palm basis did not produce a fixed direction vector.");
    require(std::abs(orientedProjection.palmDirectionTip.position.x
            - orientedProjection.palmCenter.position.x) > 20.0F,
        "The palm direction vector did not rotate with the 3D view.");

    hand.worldLandmarks[8].x = std::numeric_limits<float>::quiet_NaN();
    require(!ryoiki::rendering::projectHand3d(hand, plot, frontView).valid,
        "A non-finite world landmark produced a valid 3D projection.");
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
        testGpuPreprocessMatchesCpuReference();
        testPalmSamplingUsesSharedPixelCenterOrientation();
        testRotatedHandRegions();
        testInvalidGeometryInputs();
        testUserFacingCameraSelection();
        testLatestRenderPacketKeepsFrameAndMetadataTogether();
        testFrameOrientationNormalization();
        testFrameOrientationRemainsMetadataUntilSampling();
        testFrameTransformsShareOneViewportContract();
        testSplitViewportAndHand3dProjection();
        std::cout << "VisionCore tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
