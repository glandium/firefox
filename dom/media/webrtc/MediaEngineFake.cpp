/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaEngineFake.h"

#include "AudioSegment.h"
#include "DOMMediaStream.h"
#include "ImageContainer.h"
#include "ImageTypes.h"
#include "MediaEnginePrefs.h"
#include "MediaEngineSource.h"
#include "MediaTrackGraph.h"
#include "MediaTrackListener.h"
#include "MediaTrackConstraints.h"
#include "mozilla/dom/File.h"
#include "mozilla/MediaManager.h"
#include "mozilla/Mutex.h"
#include "mozilla/SyncRunnable.h"
#include "mozilla/UniquePtr.h"
#include "nsComponentManagerUtils.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsITimer.h"
#include "SineWaveGenerator.h"
#include "Tracing.h"
#include "VideoSegment.h"
#include "VideoUtils.h"

#ifdef MOZ_WIDGET_ANDROID
#  include "nsISupportsUtils.h"
#endif

#ifdef MOZ_WEBRTC
#  include "YuvStamper.h"
#endif

#define VIDEO_WIDTH_MIN 160
#define VIDEO_WIDTH_MAX 4096
#define VIDEO_HEIGHT_MIN 90
#define VIDEO_HEIGHT_MAX 2160
#define DEFAULT_AUDIO_TIMER_MS 10
namespace mozilla {

using namespace mozilla::gfx;
using dom::MediaSourceEnum;
using dom::MediaTrackCapabilities;
using dom::MediaTrackConstraints;
using dom::MediaTrackSettings;
using dom::VideoFacingModeEnum;

static nsString FakeVideoName() {
  // For the purpose of testing we allow to change the name of the fake device
  // by pref.
  nsAutoString cameraNameFromPref;
  nsresult rv;
  auto getPref = [&]() {
    rv = Preferences::GetString("media.getusermedia.fake-camera-name",
                                cameraNameFromPref);
  };
  if (NS_IsMainThread()) {
    getPref();
  } else {
    // Here it is preferred a "hard" block, instead of "soft" block provided
    // by sync dispatch, which allows the waiting thread to spin its event
    // loop. The latter would allow multiple enumeration requests being
    // processed out-of-order.
    RefPtr runnable = NS_NewRunnableFunction(__func__, getPref);
    SyncRunnable::DispatchToThread(GetMainThreadSerialEventTarget(), runnable);
  }

  if (NS_SUCCEEDED(rv)) {
    return std::move(cameraNameFromPref);
  }
  return u"Default Video Device"_ns;
}

/**
 * Fake video source.
 */
class MediaEngineFakeVideoSource : public MediaEngineSource {
 public:
  MediaEngineFakeVideoSource();

  static nsString GetGroupId();

  nsresult Allocate(const dom::MediaTrackConstraints& aConstraints,
                    const MediaEnginePrefs& aPrefs, uint64_t aWindowID,
                    const char** aOutBadConstraint) override;
  void SetTrack(const RefPtr<MediaTrack>& aTrack,
                const PrincipalHandle& aPrincipal) override;
  nsresult Start() override;
  nsresult Reconfigure(const dom::MediaTrackConstraints& aConstraints,
                       const MediaEnginePrefs& aPrefs,
                       const char** aOutBadConstraint) override;
  nsresult Stop() override;
  nsresult Deallocate() override;

  uint32_t GetBestFitnessDistance(
      const nsTArray<const NormalizedConstraintSet*>& aConstraintSets)
      const override;
  void GetSettings(dom::MediaTrackSettings& aOutSettings) const override;

  void GetCapabilities(
      dom::MediaTrackCapabilities& aOutCapabilities) const override;

  bool IsFake() const override { return true; }

 protected:
  ~MediaEngineFakeVideoSource() = default;

  /**
   * Called by mTimer when it's time to generate a new frame.
   */
  void GenerateFrame();

  nsCOMPtr<nsITimer> mTimer;

  RefPtr<layers::ImageContainer> mImageContainer;

  // Current state of this source.
  MediaEngineSourceState mState = kReleased;
  RefPtr<layers::Image> mImage;
  RefPtr<SourceMediaTrack> mTrack;
  PrincipalHandle mPrincipalHandle = PRINCIPAL_HANDLE_NONE;

  MediaEnginePrefs mOpts;
  int mCb = 16;
  int mCr = 16;

  // Main thread only.
  const RefPtr<media::Refcountable<dom::MediaTrackSettings>> mSettings;
};

MediaEngineFakeVideoSource::MediaEngineFakeVideoSource()
    : mTimer(nullptr),
      mSettings(MakeAndAddRef<media::Refcountable<MediaTrackSettings>>()) {
  mSettings->mWidth.Construct(
      int32_t(MediaEnginePrefs::DEFAULT_43_VIDEO_WIDTH));
  mSettings->mHeight.Construct(
      int32_t(MediaEnginePrefs::DEFAULT_43_VIDEO_HEIGHT));
  mSettings->mFrameRate.Construct(double(MediaEnginePrefs::DEFAULT_VIDEO_FPS));
  mSettings->mFacingMode.Construct(NS_ConvertASCIItoUTF16(
      dom::GetEnumString(VideoFacingModeEnum::Environment)));
}

nsString MediaEngineFakeVideoSource::GetGroupId() {
  return u"Fake Video Group"_ns;
}

uint32_t MediaEngineFakeVideoSource::GetBestFitnessDistance(
    const nsTArray<const NormalizedConstraintSet*>& aConstraintSets) const {
  AssertIsOnOwningThread();

  uint64_t distance = 0;

#ifdef MOZ_WEBRTC
  // distance is read from first entry only
  if (aConstraintSets.Length() >= 1) {
    const auto* cs = aConstraintSets.ElementAt(0);
    Maybe<nsString> facingMode = Nothing();
    distance +=
        MediaConstraintsHelper::FitnessDistance(facingMode, cs->mFacingMode);

    if (cs->mWidth.mMax < VIDEO_WIDTH_MIN ||
        cs->mWidth.mMin > VIDEO_WIDTH_MAX) {
      distance += UINT32_MAX;
    }

    if (cs->mHeight.mMax < VIDEO_HEIGHT_MIN ||
        cs->mHeight.mMin > VIDEO_HEIGHT_MAX) {
      distance += UINT32_MAX;
    }
  }
#endif

  return uint32_t(std::min(distance, uint64_t(UINT32_MAX)));
}

void MediaEngineFakeVideoSource::GetSettings(
    MediaTrackSettings& aOutSettings) const {
  MOZ_ASSERT(NS_IsMainThread());
  aOutSettings = *mSettings;
}

void MediaEngineFakeVideoSource::GetCapabilities(
    MediaTrackCapabilities& aOutCapabilities) const {
  MOZ_ASSERT(NS_IsMainThread());

  NS_ConvertASCIItoUTF16 facingString(
      GetEnumString(VideoFacingModeEnum::Environment));
  nsTArray<nsString> facing;
  facing.AppendElement(facingString);
  aOutCapabilities.mFacingMode.Construct(std::move(facing));

  dom::ULongRange widthRange;
  widthRange.mMax.Construct(VIDEO_WIDTH_MAX);
  widthRange.mMin.Construct(VIDEO_WIDTH_MIN);
  aOutCapabilities.mWidth.Construct(widthRange);

  dom::ULongRange heightRange;
  heightRange.mMax.Construct(VIDEO_HEIGHT_MAX);
  heightRange.mMin.Construct(VIDEO_HEIGHT_MIN);
  aOutCapabilities.mHeight.Construct(heightRange);

  dom::DoubleRange frameRateRange;
  frameRateRange.mMax.Construct(double(MediaEnginePrefs::DEFAULT_VIDEO_FPS));
  frameRateRange.mMin.Construct(0);
  aOutCapabilities.mFrameRate.Construct(frameRateRange);
}

nsresult MediaEngineFakeVideoSource::Allocate(
    const MediaTrackConstraints& aConstraints, const MediaEnginePrefs& aPrefs,
    uint64_t aWindowID, const char** aOutBadConstraint) {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kReleased);

  FlattenedConstraints c(aConstraints);

  // emulator debug is very, very slow; reduce load on it with smaller/slower
  // fake video
  mOpts = aPrefs;
  mOpts.mWidth =
      c.mWidth.Get(aPrefs.mWidth ? aPrefs.mWidth :
#ifdef DEBUG
                                 MediaEnginePrefs::DEFAULT_43_VIDEO_WIDTH / 2
#else
                                 MediaEnginePrefs::DEFAULT_43_VIDEO_WIDTH
#endif
      );
  mOpts.mHeight =
      c.mHeight.Get(aPrefs.mHeight ? aPrefs.mHeight :
#ifdef DEBUG
                                   MediaEnginePrefs::DEFAULT_43_VIDEO_HEIGHT / 2
#else
                                   MediaEnginePrefs::DEFAULT_43_VIDEO_HEIGHT
#endif
      );
  mOpts.mWidth =
      std::clamp(mOpts.mWidth, VIDEO_WIDTH_MIN, VIDEO_WIDTH_MAX) & ~1;
  mOpts.mHeight =
      std::clamp(mOpts.mHeight, VIDEO_HEIGHT_MIN, VIDEO_HEIGHT_MAX) & ~1;

  NS_DispatchToMainThread(NS_NewRunnableFunction(
      __func__, [settings = mSettings, frameRate = mOpts.mFPS,
                 width = mOpts.mWidth, height = mOpts.mHeight]() {
        settings->mFrameRate.Value() = frameRate;
        settings->mWidth.Value() = width;
        settings->mHeight.Value() = height;
      }));

  mState = kAllocated;
  return NS_OK;
}

nsresult MediaEngineFakeVideoSource::Deallocate() {
  AssertIsOnOwningThread();

  MOZ_ASSERT(!mImage);
  MOZ_ASSERT(mState == kStopped || mState == kAllocated);

  if (mTrack) {
    mTrack->End();
    mTrack = nullptr;
    mPrincipalHandle = PRINCIPAL_HANDLE_NONE;
  }
  mState = kReleased;
  mImageContainer = nullptr;

  return NS_OK;
}

static bool AllocateSolidColorFrame(layers::PlanarYCbCrData& aData, int aWidth,
                                    int aHeight, int aY, int aCb, int aCr) {
  MOZ_ASSERT(!(aWidth & 1));
  MOZ_ASSERT(!(aHeight & 1));
  // Allocate a single frame with a solid color
  int yLen = aWidth * aHeight;
  int cbLen = yLen >> 2;
  int crLen = cbLen;
  uint8_t* frame = (uint8_t*)malloc(yLen + cbLen + crLen);
  if (!frame) {
    return false;
  }
  memset(frame, aY, yLen);
  memset(frame + yLen, aCb, cbLen);
  memset(frame + yLen + cbLen, aCr, crLen);

  aData.mYChannel = frame;
  aData.mYStride = aWidth;
  aData.mCbCrStride = aWidth >> 1;
  aData.mCbChannel = frame + yLen;
  aData.mCrChannel = aData.mCbChannel + cbLen;
  aData.mPictureRect = IntRect(0, 0, aWidth, aHeight);
  aData.mStereoMode = StereoMode::MONO;
  aData.mYUVColorSpace = gfx::YUVColorSpace::BT601;
  aData.mChromaSubsampling = gfx::ChromaSubsampling::HALF_WIDTH_AND_HEIGHT;
  return true;
}

static void ReleaseFrame(layers::PlanarYCbCrData& aData) {
  free(aData.mYChannel);
}

void MediaEngineFakeVideoSource::SetTrack(const RefPtr<MediaTrack>& aTrack,
                                          const PrincipalHandle& aPrincipal) {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kAllocated);
  MOZ_ASSERT(!mTrack);
  MOZ_ASSERT(aTrack->AsSourceTrack());

  mTrack = aTrack->AsSourceTrack();
  mPrincipalHandle = aPrincipal;
}

nsresult MediaEngineFakeVideoSource::Start() {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kAllocated || mState == kStopped);
  MOZ_ASSERT(mTrack, "SetTrack() must happen before Start()");

  mTimer = NS_NewTimer(GetCurrentSerialEventTarget());
  if (!mTimer) {
    return NS_ERROR_FAILURE;
  }

  if (!mImageContainer) {
    mImageContainer = MakeAndAddRef<layers::ImageContainer>(
        layers::ImageUsageType::Webrtc, layers::ImageContainer::ASYNCHRONOUS);
  }

  // Start timer for subsequent frames
  const uint32_t interval = 1000 / mOpts.mFPS;
  mTimer->InitWithNamedFuncCallback(
      [](nsITimer* aTimer, void* aClosure) {
        RefPtr<MediaEngineFakeVideoSource> source =
            static_cast<MediaEngineFakeVideoSource*>(aClosure);
        source->GenerateFrame();
      },
      this, interval, nsITimer::TYPE_REPEATING_PRECISE_CAN_SKIP,
      "MediaEngineFakeVideoSource::GenerateFrame");

  mState = kStarted;
  return NS_OK;
}

nsresult MediaEngineFakeVideoSource::Stop() {
  AssertIsOnOwningThread();

  if (mState == kStopped || mState == kAllocated) {
    return NS_OK;
  }

  MOZ_ASSERT(mState == kStarted);
  MOZ_ASSERT(mTimer);
  MOZ_ASSERT(mTrack);

  mTimer->Cancel();
  mTimer = nullptr;

  mState = kStopped;

  return NS_OK;
}

nsresult MediaEngineFakeVideoSource::Reconfigure(
    const MediaTrackConstraints& aConstraints, const MediaEnginePrefs& aPrefs,
    const char** aOutBadConstraint) {
  return NS_OK;
}

void MediaEngineFakeVideoSource::GenerateFrame() {
  AssertIsOnOwningThread();

  // Update the target color
  if (mCr <= 16) {
    if (mCb < 240) {
      mCb++;
    } else {
      mCr++;
    }
  } else if (mCb >= 240) {
    if (mCr < 240) {
      mCr++;
    } else {
      mCb--;
    }
  } else if (mCr >= 240) {
    if (mCb > 16) {
      mCb--;
    } else {
      mCr--;
    }
  } else {
    mCr--;
  }

  // Allocate a single solid color image
  RefPtr<layers::PlanarYCbCrImage> ycbcr_image =
      mImageContainer->CreatePlanarYCbCrImage();
  layers::PlanarYCbCrData data;
  if (NS_WARN_IF(!AllocateSolidColorFrame(data, mOpts.mWidth, mOpts.mHeight,
                                          0x80, mCb, mCr))) {
    return;
  }

#ifdef MOZ_WEBRTC
  uint64_t timestamp = PR_Now();
  YuvStamper::Encode(mOpts.mWidth, mOpts.mHeight, mOpts.mWidth, data.mYChannel,
                     reinterpret_cast<unsigned char*>(&timestamp),
                     sizeof(timestamp), 0, 0);
#endif

  bool setData = NS_SUCCEEDED(ycbcr_image->CopyData(data));
  MOZ_ASSERT(setData);

  // SetData copies data, so we can free the frame
  ReleaseFrame(data);

  if (!setData) {
    return;
  }

  VideoSegment segment;
  segment.AppendFrame(ycbcr_image.forget(),
                      gfx::IntSize(mOpts.mWidth, mOpts.mHeight),
                      mPrincipalHandle);
  mTrack->AppendData(&segment);
}

// This class is created on the media thread, as part of Start(), then entirely
// self-sustained until destruction, just forwarding calls to Pull().
class AudioSourcePullListener : public MediaTrackListener {
 public:
  AudioSourcePullListener(RefPtr<SourceMediaTrack> aTrack,
                          const PrincipalHandle& aPrincipalHandle,
                          uint32_t aFrequency)
      : mTrack(std::move(aTrack)),
        mPrincipalHandle(aPrincipalHandle),
        mSineGenerator(MakeUnique<SineWaveGenerator<int16_t>>(
            mTrack->mSampleRate, aFrequency)) {
    MOZ_COUNT_CTOR(AudioSourcePullListener);
  }

  MOZ_COUNTED_DTOR(AudioSourcePullListener)

  void NotifyPull(MediaTrackGraph* aGraph, TrackTime aEndOfAppendedData,
                  TrackTime aDesiredTime) override;

  const RefPtr<SourceMediaTrack> mTrack;
  const PrincipalHandle mPrincipalHandle;
  const UniquePtr<SineWaveGenerator<int16_t>> mSineGenerator;
};

/**
 * Fake audio source.
 */
class MediaEngineFakeAudioSource : public MediaEngineSource {
 public:
  MediaEngineFakeAudioSource() = default;

  static nsString GetUUID();
  static nsString GetGroupId();

  nsresult Allocate(const dom::MediaTrackConstraints& aConstraints,
                    const MediaEnginePrefs& aPrefs, uint64_t aWindowID,
                    const char** aOutBadConstraint) override;
  void SetTrack(const RefPtr<MediaTrack>& aTrack,
                const PrincipalHandle& aPrincipal) override;
  nsresult Start() override;
  nsresult Reconfigure(const dom::MediaTrackConstraints& aConstraints,
                       const MediaEnginePrefs& aPrefs,
                       const char** aOutBadConstraint) override;
  nsresult Stop() override;
  nsresult Deallocate() override;

  bool IsFake() const override { return true; }

  void GetSettings(dom::MediaTrackSettings& aOutSettings) const override;

  void GetCapabilities(
      dom::MediaTrackCapabilities& aOutCapabilities) const override;

 protected:
  ~MediaEngineFakeAudioSource() = default;

  // Current state of this source.
  MediaEngineSourceState mState = kReleased;
  RefPtr<SourceMediaTrack> mTrack;
  PrincipalHandle mPrincipalHandle = PRINCIPAL_HANDLE_NONE;
  uint32_t mFrequency = 1000;
  RefPtr<AudioSourcePullListener> mPullListener;
};

nsString MediaEngineFakeAudioSource::GetUUID() {
  return u"B7CBD7C1-53EF-42F9-8353-73F61C70C092"_ns;
}

nsString MediaEngineFakeAudioSource::GetGroupId() {
  return u"Fake Audio Group"_ns;
}

void MediaEngineFakeAudioSource::GetSettings(
    MediaTrackSettings& aOutSettings) const {
  MOZ_ASSERT(NS_IsMainThread());
  aOutSettings.mAutoGainControl.Construct(false);
  aOutSettings.mEchoCancellation.Construct(false);
  aOutSettings.mNoiseSuppression.Construct(false);
  aOutSettings.mChannelCount.Construct(1);
}

void MediaEngineFakeAudioSource::GetCapabilities(
    MediaTrackCapabilities& aOutCapabilities) const {
  MOZ_ASSERT(NS_IsMainThread());
  nsTArray<bool> echoCancellation;
  echoCancellation.AppendElement(false);
  aOutCapabilities.mEchoCancellation.Construct(std::move(echoCancellation));

  nsTArray<bool> autoGainControl;
  autoGainControl.AppendElement(false);
  aOutCapabilities.mAutoGainControl.Construct(std::move(autoGainControl));

  nsTArray<bool> noiseSuppression;
  noiseSuppression.AppendElement(false);
  aOutCapabilities.mNoiseSuppression.Construct(std::move(noiseSuppression));

  dom::ULongRange channelCountRange;
  channelCountRange.mMax.Construct(1);
  channelCountRange.mMin.Construct(1);
  aOutCapabilities.mChannelCount.Construct(channelCountRange);
}

nsresult MediaEngineFakeAudioSource::Allocate(
    const MediaTrackConstraints& aConstraints, const MediaEnginePrefs& aPrefs,
    uint64_t aWindowID, const char** aOutBadConstraint) {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kReleased);

  mFrequency = aPrefs.mFreq ? aPrefs.mFreq : 1000;

  mState = kAllocated;
  return NS_OK;
}

nsresult MediaEngineFakeAudioSource::Deallocate() {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kStopped || mState == kAllocated);

  if (mTrack) {
    mTrack->End();
    mTrack = nullptr;
    mPrincipalHandle = PRINCIPAL_HANDLE_NONE;
  }
  mState = kReleased;
  return NS_OK;
}

void MediaEngineFakeAudioSource::SetTrack(const RefPtr<MediaTrack>& aTrack,
                                          const PrincipalHandle& aPrincipal) {
  AssertIsOnOwningThread();

  MOZ_ASSERT(mState == kAllocated);
  MOZ_ASSERT(!mTrack);
  MOZ_ASSERT(aTrack->AsSourceTrack());

  mTrack = aTrack->AsSourceTrack();
  mPrincipalHandle = aPrincipal;
}

nsresult MediaEngineFakeAudioSource::Start() {
  AssertIsOnOwningThread();

  if (mState == kStarted) {
    return NS_OK;
  }

  MOZ_ASSERT(mState == kAllocated || mState == kStopped);
  MOZ_ASSERT(mTrack, "SetTrack() must happen before Start()");

  if (!mPullListener) {
    mPullListener = MakeAndAddRef<AudioSourcePullListener>(
        mTrack, mPrincipalHandle, mFrequency);
  }

  mState = kStarted;

  NS_DispatchToMainThread(NS_NewRunnableFunction(
      __func__, [track = mTrack, listener = mPullListener]() {
        if (track->IsDestroyed()) {
          return;
        }
        track->AddListener(listener);
        track->SetPullingEnabled(true);
      }));

  return NS_OK;
}

nsresult MediaEngineFakeAudioSource::Stop() {
  AssertIsOnOwningThread();

  if (mState == kStopped || mState == kAllocated) {
    return NS_OK;
  }
  MOZ_ASSERT(mState == kStarted);
  mState = kStopped;

  NS_DispatchToMainThread(NS_NewRunnableFunction(
      __func__, [track = mTrack, listener = std::move(mPullListener)]() {
        if (track->IsDestroyed()) {
          return;
        }
        track->RemoveListener(listener);
        track->SetPullingEnabled(false);
      }));
  return NS_OK;
}

nsresult MediaEngineFakeAudioSource::Reconfigure(
    const MediaTrackConstraints& aConstraints, const MediaEnginePrefs& aPrefs,
    const char** aOutBadConstraint) {
  return NS_OK;
}

void AudioSourcePullListener::NotifyPull(MediaTrackGraph* aGraph,
                                         TrackTime aEndOfAppendedData,
                                         TrackTime aDesiredTime) {
  TRACE_COMMENT("SourceMediaTrack::NotifyPull", "SourceMediaTrack %p",
                mTrack.get());
  AudioSegment segment;
  TrackTicks delta = aDesiredTime - aEndOfAppendedData;
  CheckedInt<size_t> bufferSize(sizeof(int16_t));
  bufferSize *= delta;
  RefPtr<SharedBuffer> buffer = SharedBuffer::Create(bufferSize);
  int16_t* dest = static_cast<int16_t*>(buffer->Data());
  mSineGenerator->generate(dest, delta);
  AutoTArray<const int16_t*, 1> channels;
  channels.AppendElement(dest);
  segment.AppendFrames(buffer.forget(), channels, delta, mPrincipalHandle);
  mTrack->AppendData(&segment);
}

MediaEngineFake::MediaEngineFake() = default;
MediaEngineFake::~MediaEngineFake() = default;

void MediaEngineFake::EnumerateDevices(
    MediaSourceEnum aMediaSource, MediaSinkEnum aMediaSink,
    nsTArray<RefPtr<MediaDevice>>* aDevices) {
  AssertIsOnOwningThread();
  using IsScary = MediaDevice::IsScary;
  using OsPromptable = MediaDevice::OsPromptable;

  if (aMediaSink == MediaSinkEnum::Speaker) {
    NS_WARNING("No default implementation for MediaSinkEnum::Speaker");
  }

  switch (aMediaSource) {
    case MediaSourceEnum::Camera: {
      nsString name = FakeVideoName();
      aDevices->EmplaceBack(
          new MediaDevice(this, aMediaSource, name, /*aRawId=*/name,
                          MediaEngineFakeVideoSource::GetGroupId(), IsScary::No,
                          OsPromptable::No));
      return;
    }
    case MediaSourceEnum::Microphone:
      aDevices->EmplaceBack(
          new MediaDevice(this, aMediaSource, u"Default Audio Device"_ns,
                          MediaEngineFakeAudioSource::GetUUID(),
                          MediaEngineFakeAudioSource::GetGroupId(), IsScary::No,
                          OsPromptable::No));
      return;
    default:
      MOZ_ASSERT_UNREACHABLE("Unsupported source type");
      return;
  }
}

RefPtr<MediaEngineSource> MediaEngineFake::CreateSource(
    const MediaDevice* aMediaDevice) {
  MOZ_ASSERT(aMediaDevice->mEngine == this);
  switch (aMediaDevice->mMediaSource) {
    case MediaSourceEnum::Camera:
      return new MediaEngineFakeVideoSource();
    case MediaSourceEnum::Microphone:
      return new MediaEngineFakeAudioSource();
    default:
      MOZ_ASSERT_UNREACHABLE("Unsupported source type");
      return nullptr;
  }
}

}  // namespace mozilla
