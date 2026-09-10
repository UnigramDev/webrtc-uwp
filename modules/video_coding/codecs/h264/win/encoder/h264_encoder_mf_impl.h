/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_H264_WIN_ENCODER_H264_ENCODER_MF_IMPL_H_
#define MODULES_VIDEO_CODING_CODECS_H264_WIN_ENCODER_H264_ENCODER_MF_IMPL_H_

#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <deque>
#include <string>
#include <vector>

#include "api/video_codecs/h264_profile_level_id.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "rtc_base/synchronization/mutex.h"

namespace webrtc {

// H.264 encoder built directly on a Media Foundation transform.
//
// Hardware first: the MFTs registered for NV12 in and H.264 out are enumerated
// and activated in merit order, and Microsoft's software encoder is used only
// when no hardware one can be activated. Hardware encoders are asynchronous and
// are driven by transform events on a Media Foundation thread; the software one
// is synchronous and is driven from Encode(). Both are supported here because
// that is what having a fallback costs.
//
// The contract that shapes the whole class: **Encode() must not block.** It runs
// on WebRTC's encoder queue, and FrameCadenceAdapterImpl posts one task per
// captured frame to that queue holding the frame, uncapped -- so an encoder that
// waits there retains every frame the camera produces. Nothing here waits on the
// hardware, and reconfiguration reuses the transform rather than building a new
// one.
class H264EncoderMFImpl : public VideoEncoder {
 public:
  // The profile decides the transform's MF_MT_MPEG2_PROFILE, and which
  // transforms are usable at all: NVIDIA's does not honour constrained
  // baseline. It comes from the negotiated SDP format, so the factory passes it.
  explicit H264EncoderMFImpl(
      H264Profile profile = H264Profile::kProfileConstrainedBaseline);
  ~H264EncoderMFImpl() override;

  H264EncoderMFImpl(const H264EncoderMFImpl&) = delete;
  H264EncoderMFImpl& operator=(const H264EncoderMFImpl&) = delete;

  // === VideoEncoder overrides ===
  int32_t InitEncode(const VideoCodec* codec_settings,
                     const VideoEncoder::Settings& settings) override;
  int32_t Release() override;
  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;
  void SetRates(const RateControlParameters& parameters) override;
  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* frame_types) override;
  VideoEncoder::EncoderInfo GetEncoderInfo() const override;

 private:
  // Receives METransformNeedInput / METransformHaveOutput from an asynchronous
  // transform. Refcounted and detachable, because Media Foundation holds its own
  // reference and can be inside Invoke() while the encoder is being released.
  class EventCallback;

  // What has to survive the trip through the transform. Hardware encoders do not
  // carry custom sample attributes across, so this is kept beside the transform
  // and matched on the sample time, which they do carry.
  struct FrameMetadata {
    LONGLONG time_hns = 0;
    uint32_t rtp_timestamp = 0;
    int64_t ntp_time_ms = 0;
    int64_t capture_time_ms = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Carried with the frame rather than applied when the request arrives: a
    // queued frame may be fed later, and CODECAPI_AVEncVideoForceKeyFrame
    // applies to whatever the transform takes next.
    bool key_frame = false;
  };

  // One encoded frame, lifted out from under the lock so that the callback into
  // WebRTC is made with nothing held.
  struct EncodedFrame {
    EncodedImage image;
    CodecSpecificInfo codec_specific;
  };

  enum class Vendor { kOther, kIntel, kNvidia, kAmd, kQualcomm };

  // Transform lifecycle. All of these run with mutex_ held.
  int32_t InitTransform() RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  bool ActivateTransform() RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  bool ConfigureMediaTypes() RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  bool ConfigureCodecApi() RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  bool StartStreaming() RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void ReleaseTransform() RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  // Detaches the event callback and releases the transform. Takes mutex_
  // itself, and must be called with it not held.
  void ShutdownTransform();

  // Retargets the running transform at a new frame size, reusing it: creating a
  // transform costs tens of milliseconds, on the encoder queue.
  bool UpdateFrameSize(UINT32 width, UINT32 height)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  bool ApplyBitrate(UINT32 bps, UINT32 fps) RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  bool RequestKeyFrame() RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Input. CreateInputSample() does the I420 to NV12 conversion and is called
  // with no lock held: the copy is the most expensive thing on this path, and
  // holding the lock across it would stall the event thread.
  Microsoft::WRL::ComPtr<IMFSample> CreateInputSample(const VideoFrame& frame,
                                                     UINT32 width,
                                                     UINT32 height,
                                                     DWORD buffer_size,
                                                     DWORD buffer_alignment,
                                                     LONGLONG time_hns,
                                                     LONGLONG duration_hns);
  HRESULT FeedSample(const Microsoft::WRL::ComPtr<IMFSample>& sample,
                     const FrameMetadata& metadata)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  // Hands the transform one queued sample if it has asked for input. An input
  // credit is spent only when the sample is actually accepted.
  void FeedPending() RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Output. Both collect into `out` rather than calling WebRTC under the lock.
  HRESULT DrainOutputs(std::vector<EncodedFrame>* out)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  HRESULT ProcessOneOutput(std::vector<EncodedFrame>* out)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void Deliver(std::vector<EncodedFrame>* frames);
  void ReportDroppedFrame();

  // `generator` identifies which transform the event came from: one released
  // transform's outstanding BeginGetEvent can still complete after another has
  // been activated, and that event must not be acted on or re-armed.
  void OnTransformEvent(IMFMediaEventGenerator* generator,
                        MediaEventType type,
                        HRESULT status);

  LONGLONG FrameTimeHns(const VideoFrame& frame)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  bool PopMetadata(LONGLONG time_hns, FrameMetadata* out)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  mutable webrtc::Mutex mutex_;
  // Guards the callback pointer only. Taken while mutex_ is not held, and never
  // held while calling into Media Foundation.
  webrtc::Mutex callback_mutex_;

  // MFStartup and MFShutdown are refcounted per process, so shutting down after
  // a failed startup tears Media Foundation down under whoever else is using it.
  bool mf_started_ = false;

  bool inited_ RTC_GUARDED_BY(mutex_) = false;
  bool streaming_ RTC_GUARDED_BY(mutex_) = false;
  // Set when the transform reports MEError, or when feeding it fails in a way
  // that is not recoverable. Encode() rebuilds the transform when it sees this.
  bool broken_ RTC_GUARDED_BY(mutex_) = false;

  Microsoft::WRL::ComPtr<IMFActivate> activate_ RTC_GUARDED_BY(mutex_);
  Microsoft::WRL::ComPtr<IMFTransform> transform_ RTC_GUARDED_BY(mutex_);
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api_ RTC_GUARDED_BY(mutex_);
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator_
      RTC_GUARDED_BY(mutex_);
  Microsoft::WRL::ComPtr<IMFMediaType> input_type_ RTC_GUARDED_BY(mutex_);
  Microsoft::WRL::ComPtr<IMFMediaType> output_type_ RTC_GUARDED_BY(mutex_);
  // Not guarded: created in InitEncode and detached in Release, both on the
  // encoder queue, and its own lock covers the race with Invoke().
  Microsoft::WRL::ComPtr<EventCallback> event_callback_;

  DWORD input_stream_id_ RTC_GUARDED_BY(mutex_) = 0;
  DWORD output_stream_id_ RTC_GUARDED_BY(mutex_) = 0;
  DWORD input_buffer_size_ RTC_GUARDED_BY(mutex_) = 0;
  DWORD input_buffer_alignment_ RTC_GUARDED_BY(mutex_) = 0;
  DWORD output_buffer_size_ RTC_GUARDED_BY(mutex_) = 0;
  bool is_async_ RTC_GUARDED_BY(mutex_) = false;
  // BeginGetEvent may have only one call outstanding at a time; a second
  // returns MF_E_MULTIPLE_BEGIN. The first arm is followed by one re-arm per
  // event, so this stays true for the life of the transform.
  bool events_armed_ RTC_GUARDED_BY(mutex_) = false;
  bool is_hardware_ RTC_GUARDED_BY(mutex_) = false;
  bool transform_allocates_output_ RTC_GUARDED_BY(mutex_) = false;
  Vendor vendor_ RTC_GUARDED_BY(mutex_) = Vendor::kOther;
  std::string implementation_name_ RTC_GUARDED_BY(mutex_);

  // Negotiated configuration.
  UINT32 width_ RTC_GUARDED_BY(mutex_) = 0;
  UINT32 height_ RTC_GUARDED_BY(mutex_) = 0;
  // The frame rate the media type was built with. A frame rate change does not
  // renegotiate it; the bitrate is scaled instead, the way Chromium's encoder
  // does it, because renegotiating costs a stream restart for no benefit.
  UINT32 configured_fps_ RTC_GUARDED_BY(mutex_) = 30;
  UINT32 fps_ RTC_GUARDED_BY(mutex_) = 30;
  UINT32 target_bps_ RTC_GUARDED_BY(mutex_) = 0;
  UINT32 max_qp_ RTC_GUARDED_BY(mutex_) = 0;
  UINT32 gop_length_ RTC_GUARDED_BY(mutex_) = 0;
  const bool constrained_baseline_;
  VideoCodecMode mode_ RTC_GUARDED_BY(mutex_) = VideoCodecMode::kRealtimeVideo;

  // Asynchronous path only: input credits handed out by METransformNeedInput,
  // and the samples waiting for one. Bounded -- a full queue drops the frame,
  // which is what an encoder that cannot keep up is supposed to do.
  int needs_input_ RTC_GUARDED_BY(mutex_) = 0;
  std::deque<Microsoft::WRL::ComPtr<IMFSample>> pending_ RTC_GUARDED_BY(mutex_);
  std::deque<FrameMetadata> pending_metadata_ RTC_GUARDED_BY(mutex_);

  // Handed to the transform and not yet seen coming out, oldest first.
  std::deque<FrameMetadata> in_flight_ RTC_GUARDED_BY(mutex_);

  // The previous frame's RTP timestamp, and the transform clock accumulated
  // from the deltas between them. Accumulating rather than subtracting from a
  // start value is what survives the 32 bit RTP clock wrapping mid-call.
  uint32_t previous_rtp_timestamp_ RTC_GUARDED_BY(mutex_) = 0;
  bool have_start_timestamp_ RTC_GUARDED_BY(mutex_) = false;
  LONGLONG last_time_hns_ RTC_GUARDED_BY(mutex_) = 0;

  EncodedImageCallback* encoded_complete_callback_
      RTC_GUARDED_BY(callback_mutex_) = nullptr;
};

}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_CODECS_H264_WIN_ENCODER_H264_ENCODER_MF_IMPL_H_
