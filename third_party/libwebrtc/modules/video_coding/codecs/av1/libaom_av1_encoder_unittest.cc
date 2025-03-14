/*
 *  Copyright (c) 2020 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/codecs/av1/libaom_av1_encoder.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/test/create_frame_generator.h"
#include "api/test/frame_generator_interface.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/video_coding/codecs/test/encoded_video_frame_producer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "test/gmock.h"
#include "test/gtest.h"
#include "test/scoped_key_value_config.h"

namespace webrtc {
namespace {

using test::ScopedKeyValueConfig;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::SizeIs;
using ::testing::Values;

VideoCodec DefaultCodecSettings() {
  VideoCodec codec_settings;
  codec_settings.codecType = kVideoCodecAV1;
  codec_settings.width = 320;
  codec_settings.height = 180;
  codec_settings.maxFramerate = 30;
  codec_settings.startBitrate = 1000;
  codec_settings.qpMax = 63;
  return codec_settings;
}

VideoEncoder::Settings DefaultEncoderSettings() {
  return VideoEncoder::Settings(
      VideoEncoder::Capabilities(/*loss_notification=*/false),
      /*number_of_cores=*/1, /*max_payload_size=*/1200);
}

TEST(LibaomAv1EncoderTest, CanCreate) {
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  EXPECT_TRUE(encoder);
}

TEST(LibaomAv1EncoderTest, InitAndRelease) {
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  ASSERT_TRUE(encoder);
  VideoCodec codec_settings = DefaultCodecSettings();
  EXPECT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(encoder->Release(), WEBRTC_VIDEO_CODEC_OK);
}

TEST(LibaomAv1EncoderTest, NoBitrateOnTopLayerRefecltedInActiveDecodeTargets) {
  // Configure encoder with 2 temporal layers.
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  codec_settings.SetScalabilityMode(ScalabilityMode::kL1T2);
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);

  VideoEncoder::RateControlParameters rate_parameters;
  rate_parameters.framerate_fps = 30;
  rate_parameters.bitrate.SetBitrate(0, /*temporal_index=*/0, 300'000);
  rate_parameters.bitrate.SetBitrate(0, /*temporal_index=*/1, 0);
  encoder->SetRates(rate_parameters);

  std::vector<EncodedVideoFrameProducer::EncodedFrame> encoded_frames =
      EncodedVideoFrameProducer(*encoder).SetNumInputFrames(1).Encode();
  ASSERT_THAT(encoded_frames, SizeIs(1));
  ASSERT_NE(encoded_frames[0].codec_specific_info.generic_frame_info,
            std::nullopt);
  // Assuming L1T2 structure uses 1st decode target for T0 and 2nd decode target
  // for T0+T1 frames, expect only 1st decode target is active.
  EXPECT_EQ(encoded_frames[0]
                .codec_specific_info.generic_frame_info->active_decode_targets,
            0b01);
}

TEST(LibaomAv1EncoderTest,
     SpatialScalabilityInTemporalUnitReportedAsDeltaFrame) {
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  codec_settings.SetScalabilityMode(ScalabilityMode::kL2T1);
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);

  VideoEncoder::RateControlParameters rate_parameters;
  rate_parameters.framerate_fps = 30;
  rate_parameters.bitrate.SetBitrate(/*spatial_index=*/0, 0, 300'000);
  rate_parameters.bitrate.SetBitrate(/*spatial_index=*/1, 0, 300'000);
  encoder->SetRates(rate_parameters);

  std::vector<EncodedVideoFrameProducer::EncodedFrame> encoded_frames =
      EncodedVideoFrameProducer(*encoder).SetNumInputFrames(1).Encode();
  ASSERT_THAT(encoded_frames, SizeIs(2));
  EXPECT_THAT(encoded_frames[0].encoded_image._frameType,
              Eq(VideoFrameType::kVideoFrameKey));
  EXPECT_THAT(encoded_frames[1].encoded_image._frameType,
              Eq(VideoFrameType::kVideoFrameDelta));
}

TEST(LibaomAv1EncoderTest, NoBitrateOnTopSpatialLayerProduceDeltaFrames) {
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  codec_settings.SetScalabilityMode(ScalabilityMode::kL2T1);
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);

  VideoEncoder::RateControlParameters rate_parameters;
  rate_parameters.framerate_fps = 30;
  rate_parameters.bitrate.SetBitrate(/*spatial_index=*/0, 0, 300'000);
  rate_parameters.bitrate.SetBitrate(/*spatial_index=*/1, 0, 0);
  encoder->SetRates(rate_parameters);

  std::vector<EncodedVideoFrameProducer::EncodedFrame> encoded_frames =
      EncodedVideoFrameProducer(*encoder).SetNumInputFrames(2).Encode();
  ASSERT_THAT(encoded_frames, SizeIs(2));
  EXPECT_THAT(encoded_frames[0].encoded_image._frameType,
              Eq(VideoFrameType::kVideoFrameKey));
  EXPECT_THAT(encoded_frames[1].encoded_image._frameType,
              Eq(VideoFrameType::kVideoFrameDelta));
}

TEST(LibaomAv1EncoderTest, SetsEndOfPictureForLastFrameInTemporalUnit) {
  VideoBitrateAllocation allocation;
  allocation.SetBitrate(0, 0, 30000);
  allocation.SetBitrate(1, 0, 40000);
  allocation.SetBitrate(2, 0, 30000);

  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  // Configure encoder with 3 spatial layers.
  codec_settings.SetScalabilityMode(ScalabilityMode::kL3T1);
  codec_settings.startBitrate = allocation.get_sum_kbps();
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);

  encoder->SetRates(VideoEncoder::RateControlParameters(
      allocation, codec_settings.maxFramerate));

  std::vector<EncodedVideoFrameProducer::EncodedFrame> encoded_frames =
      EncodedVideoFrameProducer(*encoder).SetNumInputFrames(2).Encode();
  ASSERT_THAT(encoded_frames, SizeIs(6));
  EXPECT_FALSE(encoded_frames[0].codec_specific_info.end_of_picture);
  EXPECT_FALSE(encoded_frames[1].codec_specific_info.end_of_picture);
  EXPECT_TRUE(encoded_frames[2].codec_specific_info.end_of_picture);
  EXPECT_FALSE(encoded_frames[3].codec_specific_info.end_of_picture);
  EXPECT_FALSE(encoded_frames[4].codec_specific_info.end_of_picture);
  EXPECT_TRUE(encoded_frames[5].codec_specific_info.end_of_picture);
}

TEST(LibaomAv1EncoderTest,
     SetsEndOfPictureForLastFrameInTemporalUnitWhenLayerDrop) {
  VideoBitrateAllocation allocation;
  allocation.SetBitrate(0, 0, 30000);
  allocation.SetBitrate(1, 0, 40000);
  // Lower bitrate for the last spatial layer to provoke layer drop.
  allocation.SetBitrate(2, 0, 500);

  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  // Configure encoder with 3 spatial layers.
  codec_settings.SetScalabilityMode(ScalabilityMode::kL3T1);
  codec_settings.startBitrate = allocation.get_sum_kbps();
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);

  encoder->SetRates(VideoEncoder::RateControlParameters(
      allocation, codec_settings.maxFramerate));

  std::vector<EncodedVideoFrameProducer::EncodedFrame> encoded_frames =
      EncodedVideoFrameProducer(*encoder).SetNumInputFrames(2).Encode();
  ASSERT_THAT(encoded_frames, SizeIs(4));
  EXPECT_FALSE(encoded_frames[0].codec_specific_info.end_of_picture);
  EXPECT_TRUE(encoded_frames[1].codec_specific_info.end_of_picture);
  EXPECT_FALSE(encoded_frames[2].codec_specific_info.end_of_picture);
  EXPECT_TRUE(encoded_frames[3].codec_specific_info.end_of_picture);
}

TEST(LibaomAv1EncoderTest, CheckOddDimensionsWithSpatialLayers) {
  VideoBitrateAllocation allocation;
  allocation.SetBitrate(0, 0, 30000);
  allocation.SetBitrate(1, 0, 40000);
  allocation.SetBitrate(2, 0, 30000);
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  // Configure encoder with 3 spatial layers.
  codec_settings.SetScalabilityMode(ScalabilityMode::kL3T1);
  // Odd width and height values should not make encoder crash.
  codec_settings.width = 623;
  codec_settings.height = 405;
  codec_settings.startBitrate = allocation.get_sum_kbps();
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);
  encoder->SetRates(VideoEncoder::RateControlParameters(
      allocation, codec_settings.maxFramerate));
  EncodedVideoFrameProducer evfp(*encoder);
  evfp.SetResolution(RenderResolution{623, 405});
  std::vector<EncodedVideoFrameProducer::EncodedFrame> encoded_frames =
      evfp.SetNumInputFrames(2).Encode();
  ASSERT_THAT(encoded_frames, SizeIs(6));
}

class LibaomAv1EncoderMaxConsecDropTest
    : public ::testing::TestWithParam</*framerate_fps=*/int> {};

TEST_P(LibaomAv1EncoderMaxConsecDropTest, MaxConsecDrops) {
  VideoBitrateAllocation allocation;
  allocation.SetBitrate(0, 0,
                        1000);  // Very low bitrate to provoke frame drops.
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  codec_settings.SetFrameDropEnabled(true);
  codec_settings.SetScalabilityMode(ScalabilityMode::kL1T1);
  codec_settings.startBitrate = allocation.get_sum_kbps();
  codec_settings.maxFramerate = GetParam();
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);
  encoder->SetRates(VideoEncoder::RateControlParameters(
      allocation, codec_settings.maxFramerate));
  std::vector<EncodedVideoFrameProducer::EncodedFrame> encoded_frames =
      EncodedVideoFrameProducer(*encoder)
          .SetNumInputFrames(60)
          .SetFramerateFps(codec_settings.maxFramerate)
          .SetResolution(RenderResolution{320, 180})
          .Encode();
  ASSERT_GE(encoded_frames.size(), 2u);

  int max_consec_drops = 0;
  for (size_t i = 1; i < encoded_frames.size(); ++i) {
    uint32_t frame_duration_rtp =
        encoded_frames[i].encoded_image.RtpTimestamp() -
        encoded_frames[i - 1].encoded_image.RtpTimestamp();
    // X consecutive drops result in a freeze of (X + 1) frame duration.
    // Subtract 1 to get pure number of drops.
    int num_drops = frame_duration_rtp * codec_settings.maxFramerate /
                        kVideoPayloadTypeFrequency -
                    1;
    max_consec_drops = std::max(max_consec_drops, num_drops);
  }

  const int expected_max_consec_drops =
      std::ceil(0.25 * codec_settings.maxFramerate);
  EXPECT_EQ(max_consec_drops, expected_max_consec_drops);
}

INSTANTIATE_TEST_SUITE_P(LibaomAv1EncoderMaxConsecDropTests,
                         LibaomAv1EncoderMaxConsecDropTest,
                         Values(1, 2, 5, 15, 30, 60));

TEST(LibaomAv1EncoderTest, EncoderInfoWithoutResolutionBitrateLimits) {
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  EXPECT_TRUE(encoder->GetEncoderInfo().resolution_bitrate_limits.empty());
}

TEST(LibaomAv1EncoderTest, EncoderInfoWithBitrateLimitsFromFieldTrial) {
  auto field_trials = std::make_unique<ScopedKeyValueConfig>(
      "WebRTC-Av1-GetEncoderInfoOverride/"
      "frame_size_pixels:123|456|789,"
      "min_start_bitrate_bps:11000|22000|33000,"
      "min_bitrate_bps:44000|55000|66000,"
      "max_bitrate_bps:77000|88000|99000/");
  const Environment env = CreateEnvironment(std::move(field_trials));
  std::unique_ptr<VideoEncoder> encoder = CreateLibaomAv1Encoder(env);

  EXPECT_THAT(
      encoder->GetEncoderInfo().resolution_bitrate_limits,
      ::testing::ElementsAre(
          VideoEncoder::ResolutionBitrateLimits{123, 11000, 44000, 77000},
          VideoEncoder::ResolutionBitrateLimits{456, 22000, 55000, 88000},
          VideoEncoder::ResolutionBitrateLimits{789, 33000, 66000, 99000}));
}

TEST(LibaomAv1EncoderTest, EncoderInfoProvidesFpsAllocation) {
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  codec_settings.SetScalabilityMode(ScalabilityMode::kL3T3);
  codec_settings.maxFramerate = 60;
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);

  const auto& encoder_info = encoder->GetEncoderInfo();
  EXPECT_THAT(encoder_info.fps_allocation[0],
              ElementsAre(255 / 4, 255 / 2, 255));
  EXPECT_THAT(encoder_info.fps_allocation[1],
              ElementsAre(255 / 4, 255 / 2, 255));
  EXPECT_THAT(encoder_info.fps_allocation[2],
              ElementsAre(255 / 4, 255 / 2, 255));
  EXPECT_THAT(encoder_info.fps_allocation[3], IsEmpty());
}

TEST(LibaomAv1EncoderTest, PopulatesEncodedFrameSize) {
  VideoBitrateAllocation allocation;
  allocation.SetBitrate(0, 0, 30000);
  allocation.SetBitrate(1, 0, 40000);
  allocation.SetBitrate(2, 0, 30000);
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  codec_settings.startBitrate = allocation.get_sum_kbps();
  ASSERT_GT(codec_settings.width, 4);
  // Configure encoder with 3 spatial layers.
  codec_settings.SetScalabilityMode(ScalabilityMode::kL3T1);
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);
  encoder->SetRates(VideoEncoder::RateControlParameters(
      allocation, codec_settings.maxFramerate));
  using Frame = EncodedVideoFrameProducer::EncodedFrame;
  std::vector<Frame> encoded_frames =
      EncodedVideoFrameProducer(*encoder).SetNumInputFrames(1).Encode();
  EXPECT_THAT(
      encoded_frames,
      ElementsAre(
          Field(&Frame::encoded_image,
                AllOf(Field(&EncodedImage::_encodedWidth,
                            codec_settings.width / 4),
                      Field(&EncodedImage::_encodedHeight,
                            codec_settings.height / 4))),
          Field(&Frame::encoded_image,
                AllOf(Field(&EncodedImage::_encodedWidth,
                            codec_settings.width / 2),
                      Field(&EncodedImage::_encodedHeight,
                            codec_settings.height / 2))),
          Field(&Frame::encoded_image,
                AllOf(Field(&EncodedImage::_encodedWidth, codec_settings.width),
                      Field(&EncodedImage::_encodedHeight,
                            codec_settings.height)))));
}

TEST(LibaomAv1EncoderTest, RtpTimestampWrap) {
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  codec_settings.SetScalabilityMode(ScalabilityMode::kL1T1);
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);

  VideoEncoder::RateControlParameters rate_parameters;
  rate_parameters.framerate_fps = 30;
  rate_parameters.bitrate.SetBitrate(/*spatial_index=*/0, 0, 300'000);
  encoder->SetRates(rate_parameters);

  std::vector<EncodedVideoFrameProducer::EncodedFrame> encoded_frames =
      EncodedVideoFrameProducer(*encoder)
          .SetNumInputFrames(2)
          .SetRtpTimestamp(std::numeric_limits<uint32_t>::max())
          .Encode();
  ASSERT_THAT(encoded_frames, SizeIs(2));
  EXPECT_THAT(encoded_frames[0].encoded_image._frameType,
              Eq(VideoFrameType::kVideoFrameKey));
  EXPECT_THAT(encoded_frames[1].encoded_image._frameType,
              Eq(VideoFrameType::kVideoFrameDelta));
}

TEST(LibaomAv1EncoderTest, TestPresentationTimestamp) {
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  const Timestamp presentation_timestamp = Timestamp::Micros(2000);
  VideoCodec codec_settings = DefaultCodecSettings();
  codec_settings.SetScalabilityMode(ScalabilityMode::kL2T1);
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);

  VideoEncoder::RateControlParameters rate_parameters;
  rate_parameters.framerate_fps = 30;
  rate_parameters.bitrate.SetBitrate(/*spatial_index=*/0, /*temporal_index=*/0,
                                     300'000);
  rate_parameters.bitrate.SetBitrate(/*spatial_index=*/1, /*temporal_index=*/0,
                                     300'000);
  encoder->SetRates(rate_parameters);

  std::vector<EncodedVideoFrameProducer::EncodedFrame> encoded_frames =
      EncodedVideoFrameProducer(*encoder)
          .SetNumInputFrames(1)
          .SetPresentationTimestamp(presentation_timestamp)
          .Encode();
  ASSERT_THAT(encoded_frames, SizeIs(2));
  ASSERT_TRUE(
      encoded_frames[0].encoded_image.PresentationTimestamp().has_value());
  ASSERT_TRUE(
      encoded_frames[1].encoded_image.PresentationTimestamp().has_value());
  EXPECT_EQ(encoded_frames[0].encoded_image.PresentationTimestamp()->us(),
            presentation_timestamp.us());
  EXPECT_EQ(encoded_frames[1].encoded_image.PresentationTimestamp()->us(),
            presentation_timestamp.us());
}

TEST(LibaomAv1EncoderTest, AdheresToTargetBitrateDespiteUnevenFrameTiming) {
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  VideoCodec codec_settings = DefaultCodecSettings();
  codec_settings.SetScalabilityMode(ScalabilityMode::kL1T1);
  codec_settings.startBitrate = 300;  // kbps
  codec_settings.width = 320;
  codec_settings.height = 180;
  ASSERT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);

  const int kFps = 30;
  const int kTargetBitrateBps = codec_settings.startBitrate * 1000;
  VideoEncoder::RateControlParameters rate_parameters;
  rate_parameters.framerate_fps = kFps;
  rate_parameters.bitrate.SetBitrate(/*spatial_index=*/0, 0, kTargetBitrateBps);
  encoder->SetRates(rate_parameters);

  class EncoderCallback : public EncodedImageCallback {
   public:
    EncoderCallback() = default;
    DataSize BytesEncoded() const { return bytes_encoded_; }

   private:
    Result OnEncodedImage(
        const EncodedImage& encoded_image,
        const CodecSpecificInfo* /* codec_specific_info */) override {
      bytes_encoded_ += DataSize::Bytes(encoded_image.size());
      return Result(Result::Error::OK);
    }

    DataSize bytes_encoded_ = DataSize::Zero();
  } callback;
  encoder->RegisterEncodeCompleteCallback(&callback);

  // Insert frames with too low rtp timestamp delta compared to what is expected
  // based on the framerate, then insert on with 2x the delta it should - making
  // the average correct.
  const uint32_t kHighTimestampDelta =
      static_cast<uint32_t>((90000.0 / kFps) * 2 + 0.5);
  const uint32_t kLowTimestampDelta =
      static_cast<uint32_t>((90000.0 - kHighTimestampDelta) / (kFps - 1));

  std::unique_ptr<test::FrameGeneratorInterface> frame_buffer_generator =
      test::CreateSquareFrameGenerator(
          codec_settings.width, codec_settings.height,
          test::FrameGeneratorInterface::OutputType::kI420, /*num_squares=*/20);

  uint32_t rtp_timestamp = 1000;
  std::vector<VideoFrameType> frame_types = {VideoFrameType::kVideoFrameKey};

  const int kRunTimeSeconds = 3;
  for (int i = 0; i < kRunTimeSeconds; ++i) {
    for (int j = 0; j < kFps; ++j) {
      if (j < kFps - 1) {
        rtp_timestamp += kLowTimestampDelta;
      } else {
        rtp_timestamp += kHighTimestampDelta;
      }
      VideoFrame frame = VideoFrame::Builder()
                             .set_video_frame_buffer(
                                 frame_buffer_generator->NextFrame().buffer)
                             .set_rtp_timestamp(rtp_timestamp)
                             .build();

      RTC_CHECK_EQ(encoder->Encode(frame, &frame_types), WEBRTC_VIDEO_CODEC_OK);
      frame_types[0] = VideoFrameType::kVideoFrameDelta;
    }
  }

  // Expect produced bitrate to match, to within 10%.
  // This catches an issue that was seen when real frame timestamps with jitter
  // was used. It resulted in the overall produced bitrate to be overshot by
  // ~30% even though the averages should have been ok.
  EXPECT_NEAR(
      (callback.BytesEncoded() / TimeDelta::Seconds(kRunTimeSeconds)).bps(),
      kTargetBitrateBps, kTargetBitrateBps / 10);
}

TEST(LibaomAv1EncoderTest, DisableAutomaticResize) {
  std::unique_ptr<VideoEncoder> encoder =
      CreateLibaomAv1Encoder(CreateEnvironment());
  ASSERT_TRUE(encoder);
  VideoCodec codec_settings = DefaultCodecSettings();
  codec_settings.AV1()->automatic_resize_on = false;
  EXPECT_EQ(encoder->InitEncode(&codec_settings, DefaultEncoderSettings()),
            WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(encoder->GetEncoderInfo().scaling_settings.thresholds,
            std::nullopt);
}

}  // namespace
}  // namespace webrtc
