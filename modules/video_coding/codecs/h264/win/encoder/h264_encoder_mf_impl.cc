/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// The Media Foundation protocol followed here -- transform enumeration and
// activation, the asynchronous unlock, the order the media types have to be
// set in, the ICodecAPI configuration, and the resolution change that reuses
// the transform -- follows Chromium's
// media/gpu/windows/media_foundation_video_encode_accelerator_win.cc, which is
// under the same BSD-3 licence. The vendor quirks are attributed inline.

#include "modules/video_coding/codecs/h264/win/encoder/h264_encoder_mf_impl.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <wrl/implements.h>

#include <algorithm>
#include <utility>

#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "libyuv/convert.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "rtc_base/logging.h"
#include "rtc_base/string_utils.h"
#include "rtc_base/time_utils.h"

using Microsoft::WRL::ComPtr;

namespace webrtc {
namespace {

// QP scaling thresholds, unchanged from the previous implementation.
constexpr int kLowH264QpThreshold = 24;
constexpr int kHighH264QpThreshold = 37;
constexpr UINT32 kMaxH264Qp = 51;

// Media Foundation counts time in 100ns units.
constexpr LONGLONG kOneSecondInHns = 10'000'000;
// H.264 RTP timestamps are 90 kHz: hns = rtp / 90'000 * 10'000'000.
constexpr LONGLONG kRtpToHnsNumerator = 10'000;
constexpr LONGLONG kRtpToHnsDenominator = 90;

// A frame arriving while this many are already waiting for the transform to ask
// for input is dropped rather than queued. The asynchronous transforms hand out
// one input credit at a time and normally keep one or two frames in flight, so
// this only fills when the hardware has stopped taking input, and then dropping
// is the correct answer: WebRTC's encoder queue must not become a frame buffer.
constexpr size_t kMaxPendingInputs = 3;

// Bounds the drain loop so a transform that keeps claiming to have output
// cannot hold the encoder queue forever.
constexpr int kMaxOutputsPerDrain = 32;

// Frames handed to the transform and not yet seen coming out. A transform that
// takes input and produces nothing would otherwise accumulate one entry per
// captured frame for the length of the call.
constexpr size_t kMaxInFlightFrames = 64;

// GOP length when the codec settings do not ask for one. Realtime video relies
// on receiver-driven key frame requests, so this is deliberately long.
constexpr UINT32 kDefaultGopLength = 3000;

// NV12 has no odd dimensions, and neither do the encoders. Rounding down and
// cropping the copy is preferable to padding: it never invents pixel data.
UINT32 RoundDownToEven(UINT32 value) {
  return value & ~UINT32{1};
}

std::string HrToString(HRESULT hr) {
  return "0x" + rtc::ToHex(hr);
}

// Bitrate is meaningful per second, but a transform configured for one frame
// rate and fed another delivers proportionally more or fewer bits per second.
// Scaling the requested bitrate keeps the output near the target without
// renegotiating the media type. From Chromium's AdjustBitrateToFrameRate.
UINT32 AdjustBitrateToFrameRate(UINT32 bitrate,
                                UINT32 configured_frame_rate,
                                UINT32 requested_frame_rate) {
  if (requested_frame_rate == 0 || configured_frame_rate == 0) {
    return bitrate;
  }
  return static_cast<UINT32>(static_cast<uint64_t>(bitrate) *
                             configured_frame_rate / requested_frame_rate);
}

}  // namespace

// Media Foundation delivers transform events on its own work queue thread. The
// callback outlives the encoder as far as Media Foundation is concerned -- it
// holds a reference through BeginGetEvent -- so the encoder detaches instead of
// destroying it.
class H264EncoderMFImpl::EventCallback
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFAsyncCallback> {
 public:
  explicit EventCallback(H264EncoderMFImpl* owner) : owner_(owner) {}

  // Blocks until any Invoke() in flight has returned, after which no further
  // call reaches the owner. **The caller must not hold the encoder's mutex_**:
  // Invoke() takes it, so holding it here would deadlock the two threads
  // against each other.
  void Detach() {
    webrtc::MutexLock lock(&lock_);
    owner_ = nullptr;
  }

  IFACEMETHODIMP GetParameters(DWORD* flags, DWORD* queue) override {
    // Documented as the correct answer for a callback with no special
    // scheduling requirements.
    return E_NOTIMPL;
  }

  IFACEMETHODIMP Invoke(IMFAsyncResult* result) override {
    ComPtr<IUnknown> state;
    ComPtr<IMFMediaEventGenerator> generator;
    ComPtr<IMFMediaEvent> media_event;

    // The generator is carried as the async state so that the event can be
    // completed without touching the owner, which may already be detached.
    HRESULT hr = result->GetState(&state);
    if (SUCCEEDED(hr)) {
      hr = state.As(&generator);
    }
    if (SUCCEEDED(hr)) {
      hr = generator->EndGetEvent(result, &media_event);
    }
    if (FAILED(hr)) {
      return S_OK;
    }

    MediaEventType type = MEUnknown;
    HRESULT status = S_OK;
    media_event->GetType(&type);
    media_event->GetStatus(&status);

    webrtc::MutexLock lock(&lock_);
    if (owner_ != nullptr) {
      owner_->OnTransformEvent(generator.Get(), type, status);
    }
    return S_OK;
  }

 private:
  webrtc::Mutex lock_;
  H264EncoderMFImpl* owner_ RTC_GUARDED_BY(lock_);
};

H264EncoderMFImpl::H264EncoderMFImpl(H264Profile profile)
    : constrained_baseline_(profile ==
                            H264Profile::kProfileConstrainedBaseline) {
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  mf_started_ = SUCCEEDED(hr);
  if (!mf_started_) {
    RTC_LOG(LS_ERROR) << "MFStartup failed: " << HrToString(hr);
  }
}

H264EncoderMFImpl::~H264EncoderMFImpl() {
  Release();
  if (mf_started_) {
    MFShutdown();
  }
}

int32_t H264EncoderMFImpl::InitEncode(const VideoCodec* codec_settings,
                                      const VideoEncoder::Settings& settings) {
  if (!mf_started_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!codec_settings || codec_settings->codecType != kVideoCodecH264) {
    RTC_LOG(LS_ERROR) << "Not registered as an H264 codec";
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (codec_settings->maxFramerate == 0) {
    RTC_LOG(LS_ERROR) << "No frame rate defined";
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (codec_settings->width < 2 || codec_settings->height < 2) {
    RTC_LOG(LS_ERROR) << "No valid frame size defined";
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  // Idempotent. Deliberately not Release(): WebRTC may have registered the
  // encoded-image callback already, and dropping it here would silence the
  // encoder for the whole stream.
  ShutdownTransform();

  webrtc::MutexLock lock(&mutex_);

  width_ = RoundDownToEven(codec_settings->width);
  height_ = RoundDownToEven(codec_settings->height);

  // WebRTC only passes the maximum frame rate, so it is also the initial one.
  fps_ = codec_settings->maxFramerate;
  configured_fps_ = fps_;
  max_qp_ = std::min<UINT32>(codec_settings->qpMax, kMaxH264Qp);
  mode_ = codec_settings->mode;

  const int key_frame_interval = codec_settings->H264().keyFrameInterval;
  gop_length_ = key_frame_interval > 0 ? static_cast<UINT32>(key_frame_interval)
                                       : kDefaultGopLength;

  if (codec_settings->startBitrate > 0) {
    target_bps_ = codec_settings->startBitrate * 1000;
  } else if (codec_settings->minBitrate > 0) {
    target_bps_ = codec_settings->minBitrate * 1000;
  } else {
    target_bps_ = width_ * height_ * 2;
  }

  have_start_timestamp_ = false;
  last_time_hns_ = 0;
  broken_ = false;

  const int32_t result = InitTransform();
  if (result != WEBRTC_VIDEO_CODEC_OK) {
    ReleaseTransform();
    return result;
  }

  inited_ = true;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t H264EncoderMFImpl::InitTransform() {
  if (!ActivateTransform()) {
    RTC_LOG(LS_ERROR) << "No H.264 encoder transform could be activated";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (!ConfigureMediaTypes()) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (!ConfigureCodecApi()) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (!StartStreaming()) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  RTC_LOG(LS_INFO) << "H.264 encoder: " << implementation_name_ << ", "
                   << (is_hardware_ ? "hardware" : "software") << ", "
                   << (is_async_ ? "async" : "sync") << ", " << width_ << "x"
                   << height_ << "@" << configured_fps_ << " "
                   << target_bps_ / 1000 << "kbps";
  return WEBRTC_VIDEO_CODEC_OK;
}

bool H264EncoderMFImpl::ActivateTransform() {
  MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};

  // Hardware first, then anything registered. The second pass is what picks up
  // Microsoft's software encoder on machines with no hardware H.264, and it is
  // the reason this class supports synchronous transforms at all.
  const UINT32 passes[] = {
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
      MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
          MFT_ENUM_FLAG_SORTANDFILTER,
  };

  for (size_t pass = 0; pass < sizeof(passes) / sizeof(passes[0]); ++pass) {
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, passes[pass],
                           &input_info, &output_info, &activates, &count);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MFTEnumEx failed: " << HrToString(hr);
      continue;
    }

    for (UINT32 i = 0; i < count; ++i) {
      ComPtr<IMFActivate> activate(activates[i]);

      Vendor vendor = Vendor::kOther;
      LPWSTR vendor_id = nullptr;
      UINT32 vendor_id_length = 0;
      if (SUCCEEDED(activate->GetAllocatedString(
              MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &vendor_id,
              &vendor_id_length))) {
        const std::wstring id(vendor_id, vendor_id_length);
        if (id == L"VEN_8086") {
          vendor = Vendor::kIntel;
        } else if (id == L"VEN_10DE") {
          vendor = Vendor::kNvidia;
        } else if (id == L"VEN_1002") {
          vendor = Vendor::kAmd;
        } else if (id == L"VEN_QCOM" || id == L"VEN_5143") {
          vendor = Vendor::kQualcomm;
        }
        CoTaskMemFree(vendor_id);
      }

      // NVIDIA's transform does not honour constrained baseline, so a stream
      // negotiated as constrained baseline comes out non-conformant. Chromium
      // skips it for the same reason, crbug.com/1088650.
      if (constrained_baseline_ && vendor == Vendor::kNvidia) {
        RTC_LOG(LS_INFO) << "Skipping the NVIDIA encoder for constrained "
                            "baseline, crbug.com/1088650";
        continue;
      }

      ComPtr<IMFTransform> transform;
      hr = activate->ActivateObject(IID_PPV_ARGS(&transform));
      if (FAILED(hr) || transform == nullptr) {
        // Whoever calls ActivateObject owns the matching ShutdownObject, even
        // when the activation failed.
        activate->ShutdownObject();
        continue;
      }

      ComPtr<IMFAttributes> attributes;
      UINT32 is_async = FALSE;
      if (SUCCEEDED(transform->GetAttributes(&attributes)) &&
          attributes != nullptr) {
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
        if (is_async) {
          // An asynchronous transform stays locked until this is set, and
          // rejects everything in the meantime.
          hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
          if (FAILED(hr)) {
            RTC_LOG(LS_WARNING) << "Couldn't unlock an async transform: "
                                << HrToString(hr);
            activate->ShutdownObject();
            continue;
          }
        }
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
      }

      if (is_async) {
        ComPtr<IMFMediaEventGenerator> generator;
        hr = transform.As(&generator);
        if (FAILED(hr)) {
          RTC_LOG(LS_WARNING)
              << "An async transform without an event generator";
          activate->ShutdownObject();
          continue;
        }
        event_generator_ = generator;
      }

      LPWSTR friendly_name = nullptr;
      UINT32 friendly_name_length = 0;
      if (SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                 &friendly_name,
                                                 &friendly_name_length))) {
        implementation_name_ =
            rtc::ToUtf8(friendly_name, friendly_name_length);
        CoTaskMemFree(friendly_name);
      } else {
        implementation_name_ = "Unknown MFT";
      }

      activate_ = activate;
      transform_ = transform;
      vendor_ = vendor;
      is_async_ = is_async != FALSE;
      is_hardware_ = pass == 0;

      DWORD input_count = 0;
      DWORD output_count = 0;
      if (SUCCEEDED(transform_->GetStreamCount(&input_count, &output_count)) &&
          input_count >= 1 && output_count >= 1) {
        std::vector<DWORD> input_ids(input_count, 0);
        std::vector<DWORD> output_ids(output_count, 0);
        hr = transform_->GetStreamIDs(input_count, input_ids.data(),
                                      output_count, output_ids.data());
        if (hr == S_OK) {
          input_stream_id_ = input_ids[0];
          output_stream_id_ = output_ids[0];
        } else {
          // E_NOTIMPL means the transform uses fixed ids starting at zero.
          input_stream_id_ = 0;
          output_stream_id_ = 0;
        }
      }

      for (UINT32 j = i + 1; j < count; ++j) {
        activates[j]->Release();
      }
      CoTaskMemFree(activates);
      return true;
    }

    for (UINT32 i = 0; i < count; ++i) {
      activates[i]->Release();
    }
    CoTaskMemFree(activates);
  }

  return false;
}

bool H264EncoderMFImpl::ConfigureMediaTypes() {
  HRESULT hr = MFCreateMediaType(&output_type_);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFCreateMediaType failed: " << HrToString(hr);
    return false;
  }

  output_type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  output_type_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  // A zero average bitrate upsets several encoders, and a transform configured
  // that way produces nothing usable. This is why the rate is carried into
  // every reconfiguration rather than read from a pending-rate field.
  output_type_->SetUINT32(MF_MT_AVG_BITRATE, std::max<UINT32>(target_bps_, 1));
  MFSetAttributeRatio(output_type_.Get(), MF_MT_FRAME_RATE, configured_fps_, 1);
  MFSetAttributeSize(output_type_.Get(), MF_MT_FRAME_SIZE, width_, height_);
  output_type_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  output_type_->SetUINT32(MF_MT_MPEG2_PROFILE,
                          constrained_baseline_
                              ? eAVEncH264VProfile_ConstrainedBase
                              : eAVEncH264VProfile_Base);

  hr = transform_->SetOutputType(output_stream_id_, output_type_.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "SetOutputType failed: " << HrToString(hr);
    return false;
  }

  // The output type has to be set first: the set of input types a video
  // encoder accepts depends on it.
  hr = MFCreateMediaType(&input_type_);
  if (FAILED(hr)) {
    return false;
  }

  input_type_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input_type_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeRatio(input_type_.Get(), MF_MT_FRAME_RATE, configured_fps_, 1);
  MFSetAttributeSize(input_type_.Get(), MF_MT_FRAME_SIZE, width_, height_);
  input_type_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  hr = transform_->SetInputType(input_stream_id_, input_type_.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "SetInputType failed: " << HrToString(hr);
    return false;
  }

  MFT_INPUT_STREAM_INFO input_info = {};
  if (SUCCEEDED(transform_->GetInputStreamInfo(input_stream_id_, &input_info))) {
    input_buffer_size_ = input_info.cbSize;
    input_buffer_alignment_ = input_info.cbAlignment;
  } else {
    input_buffer_size_ = 0;
    input_buffer_alignment_ = 0;
  }
  if (input_buffer_size_ == 0) {
    input_buffer_size_ = width_ * height_ * 3 / 2;
  }

  MFT_OUTPUT_STREAM_INFO output_info = {};
  if (SUCCEEDED(
          transform_->GetOutputStreamInfo(output_stream_id_, &output_info))) {
    transform_allocates_output_ =
        (output_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    output_buffer_size_ = output_info.cbSize;
  } else {
    transform_allocates_output_ = is_async_;
    output_buffer_size_ = 0;
  }
  if (!transform_allocates_output_ && output_buffer_size_ == 0) {
    output_buffer_size_ = width_ * height_ * 3 / 2;
  }

  return true;
}

bool H264EncoderMFImpl::ConfigureCodecApi() {
  HRESULT hr = transform_.As(&codec_api_);
  if (FAILED(hr) || codec_api_ == nullptr) {
    // Not fatal on its own: the media type carries a bitrate, so the transform
    // encodes. It does mean SetRates cannot retarget it, which is worth saying.
    RTC_LOG(LS_WARNING) << "No ICodecAPI on the encoder transform: "
                        << HrToString(hr);
    codec_api_ = nullptr;
    return true;
  }

  VARIANT var;
  var.vt = VT_UI4;
  var.ulVal = eAVEncCommonRateControlMode_CBR;
  hr = codec_api_->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Couldn't set CBR: " << HrToString(hr);
  }

  // Intel's transform wants the temporal layer count set explicitly even when
  // it is one, per Chromium's encoder.
  if (vendor_ == Vendor::kIntel) {
    var.ulVal = 1;
    codec_api_->SetValue(&CODECAPI_AVEncVideoTemporalLayerCount, &var);
  }

  var.ulVal = std::max<UINT32>(target_bps_, 1);
  hr = codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Couldn't set the mean bitrate: " << HrToString(hr);
  }

  var.ulVal = gop_length_;
  codec_api_->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);

  if (max_qp_ > 0) {
    var.ulVal = max_qp_;
    codec_api_->SetValue(&CODECAPI_AVEncVideoMaxQP, &var);
  }

  if (codec_api_->IsModifiable(&CODECAPI_AVEncAdaptiveMode) == S_OK) {
    var.ulVal = eAVEncAdaptiveMode_Resolution;
    codec_api_->SetValue(&CODECAPI_AVEncAdaptiveMode, &var);
  }

  // Qualcomm's transform emits B-frames unless told not to, and B-frames
  // reorder output, which this class does not handle and a realtime call does
  // not want. Chromium disables them for the same reason, crbug.com/343748806.
  if (vendor_ == Vendor::kQualcomm) {
    var.ulVal = 0;
    codec_api_->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);
  }

  if (codec_api_->IsModifiable(&CODECAPI_AVLowLatencyMode) == S_OK) {
    VARIANT low_latency;
    low_latency.vt = VT_BOOL;
    low_latency.boolVal = VARIANT_TRUE;
    codec_api_->SetValue(&CODECAPI_AVLowLatencyMode, &low_latency);
  }

  return true;
}

bool H264EncoderMFImpl::StartStreaming() {
  HRESULT hr = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  if (SUCCEEDED(hr)) {
    hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  }
  if (SUCCEEDED(hr)) {
    hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Couldn't start streaming: " << HrToString(hr);
    return false;
  }

  needs_input_ = 0;
  pending_.clear();
  pending_metadata_.clear();
  in_flight_.clear();
  streaming_ = true;

  // Only the first start arms the event loop. Every event re-arms it, so a
  // second BeginGetEvent here -- StartStreaming also runs on a resolution
  // change -- would be a second outstanding call and fail MF_E_MULTIPLE_BEGIN.
  if (is_async_ && !events_armed_) {
    if (event_callback_ == nullptr) {
      event_callback_ = Microsoft::WRL::Make<EventCallback>(this);
    }
    ComPtr<IMFMediaEventGenerator> generator = event_generator_;
    if (generator != nullptr && event_callback_ != nullptr) {
      // Safe to arm under the lock: BeginGetEvent queues the callback onto a
      // Media Foundation work queue, it never invokes it inline.
      hr = generator->BeginGetEvent(event_callback_.Get(), generator.Get());
      if (FAILED(hr)) {
        RTC_LOG(LS_ERROR) << "BeginGetEvent failed: " << HrToString(hr);
        return false;
      }
      events_armed_ = true;
    }
  }

  return true;
}

void H264EncoderMFImpl::ReleaseTransform() {
  if (transform_ != nullptr) {
    // Notifications only: nothing here waits for the transform to drain. A
    // drain is what put the previous implementation on the encoder queue for an
    // unbounded time.
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }

  codec_api_.Reset();
  event_generator_.Reset();
  transform_.Reset();
  input_type_.Reset();
  output_type_.Reset();

  if (activate_ != nullptr) {
    activate_->ShutdownObject();
    activate_.Reset();
  }

  pending_.clear();
  pending_metadata_.clear();
  in_flight_.clear();
  needs_input_ = 0;
  streaming_ = false;
  events_armed_ = false;
  is_async_ = false;
  is_hardware_ = false;
  transform_allocates_output_ = false;
  vendor_ = Vendor::kOther;
  implementation_name_.clear();
}

void H264EncoderMFImpl::ShutdownTransform() {
  // Before the lock: Invoke() takes mutex_, so detaching while holding it would
  // deadlock this thread against the Media Foundation event thread.
  ComPtr<EventCallback> callback = event_callback_;
  if (callback != nullptr) {
    callback->Detach();
  }
  event_callback_.Reset();

  webrtc::MutexLock lock(&mutex_);
  ReleaseTransform();
  inited_ = false;
  broken_ = false;
}

int32_t H264EncoderMFImpl::Release() {
  ShutdownTransform();

  {
    webrtc::MutexLock lock(&callback_mutex_);
    encoded_complete_callback_ = nullptr;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t H264EncoderMFImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  webrtc::MutexLock lock(&callback_mutex_);
  encoded_complete_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

void H264EncoderMFImpl::SetRates(const RateControlParameters& parameters) {
  const uint32_t bps = parameters.bitrate.get_sum_bps();
  const uint32_t fps =
      parameters.framerate_fps >= 1.0 ? static_cast<uint32_t>(
                                            parameters.framerate_fps + 0.5)
                                      : 1;

  webrtc::MutexLock lock(&mutex_);
  if (!inited_ || transform_ == nullptr) {
    return;
  }
  // WebRTC hands out a token bitrate during setup; applying it would make the
  // encoder produce nothing worth sending.
  if (bps <= 1) {
    return;
  }
  if (bps == target_bps_ && fps == fps_) {
    return;
  }

  target_bps_ = bps;
  fps_ = fps;
  ApplyBitrate(target_bps_, fps_);
}

bool H264EncoderMFImpl::ApplyBitrate(UINT32 bps, UINT32 fps) {
  if (codec_api_ == nullptr) {
    return false;
  }

  // Retargeting the running transform, never rebuilding it. The previous
  // implementation tore the pipeline down and built a new one on every rate
  // change worth more than ten percent, on the encoder queue.
  VARIANT var;
  var.vt = VT_UI4;
  var.ulVal = std::max<UINT32>(AdjustBitrateToFrameRate(bps, configured_fps_,
                                                        fps),
                               1);
  const HRESULT hr = codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate,
                                          &var);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Couldn't update the bitrate: " << HrToString(hr);
    return false;
  }
  return true;
}

bool H264EncoderMFImpl::RequestKeyFrame() {
  if (codec_api_ == nullptr) {
    return false;
  }
  VARIANT var;
  var.vt = VT_UI4;
  var.ulVal = TRUE;
  return SUCCEEDED(
      codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var));
}

bool H264EncoderMFImpl::UpdateFrameSize(UINT32 width, UINT32 height) {
  // Anything already handed to the transform belongs to the old size.
  pending_.clear();
  pending_metadata_.clear();
  in_flight_.clear();
  needs_input_ = 0;

  // Intel and Qualcomm transforms fail the first frame after a resolution
  // change without this, per Chromium's UpdateFrameSize.
  if (vendor_ == Vendor::kIntel || vendor_ == Vendor::kQualcomm) {
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }

  HRESULT hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  if (SUCCEEDED(hr)) {
    hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  }
  if (SUCCEEDED(hr)) {
    hr = transform_->SetInputType(input_stream_id_, nullptr, 0);
  }
  if (SUCCEEDED(hr)) {
    hr = transform_->SetOutputType(output_stream_id_, nullptr, 0);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Couldn't stop the transform to resize: "
                        << HrToString(hr);
    return false;
  }

  width_ = width;
  height_ = height;

  MFSetAttributeSize(output_type_.Get(), MF_MT_FRAME_SIZE, width_, height_);
  hr = transform_->SetOutputType(output_stream_id_, output_type_.Get(), 0);
  if (SUCCEEDED(hr)) {
    MFSetAttributeSize(input_type_.Get(), MF_MT_FRAME_SIZE, width_, height_);
    hr = transform_->SetInputType(input_stream_id_, input_type_.Get(), 0);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Couldn't resize the transform: " << HrToString(hr);
    return false;
  }

  MFT_INPUT_STREAM_INFO input_info = {};
  if (SUCCEEDED(transform_->GetInputStreamInfo(input_stream_id_, &input_info))) {
    input_buffer_size_ = input_info.cbSize;
    input_buffer_alignment_ = input_info.cbAlignment;
  }
  if (input_buffer_size_ == 0) {
    input_buffer_size_ = width_ * height_ * 3 / 2;
  }

  MFT_OUTPUT_STREAM_INFO output_info = {};
  if (SUCCEEDED(
          transform_->GetOutputStreamInfo(output_stream_id_, &output_info))) {
    output_buffer_size_ = output_info.cbSize;
  }
  if (!transform_allocates_output_ && output_buffer_size_ == 0) {
    output_buffer_size_ = width_ * height_ * 3 / 2;
  }

  return StartStreaming();
}

LONGLONG H264EncoderMFImpl::FrameTimeHns(const VideoFrame& frame) {
  const uint32_t rtp = frame.timestamp();
  if (!have_start_timestamp_) {
    have_start_timestamp_ = true;
    previous_rtp_timestamp_ = rtp;
    last_time_hns_ = 0;
    return 0;
  }

  // Unsigned arithmetic, so a wrap of the 32 bit RTP clock -- one every 13
  // hours at 90 kHz -- is a small delta rather than a jump backwards.
  const uint32_t delta_rtp = rtp - previous_rtp_timestamp_;
  previous_rtp_timestamp_ = rtp;
  last_time_hns_ +=
      static_cast<LONGLONG>(delta_rtp) * kRtpToHnsNumerator / kRtpToHnsDenominator;
  return last_time_hns_;
}

ComPtr<IMFSample> H264EncoderMFImpl::CreateInputSample(const VideoFrame& frame,
                                                       UINT32 width,
                                                       UINT32 height,
                                                       DWORD buffer_size,
                                                       DWORD buffer_alignment,
                                                       LONGLONG time_hns,
                                                       LONGLONG duration_hns) {
  rtc::scoped_refptr<I420BufferInterface> i420 =
      frame.video_frame_buffer()->ToI420();
  if (i420 == nullptr) {
    return nullptr;
  }

  ComPtr<IMFSample> sample;
  HRESULT hr = MFCreateSample(&sample);
  if (FAILED(hr)) {
    return nullptr;
  }

  ComPtr<IMFMediaBuffer> buffer;
  hr = MFCreateAlignedMemoryBuffer(
      buffer_size, buffer_alignment == 0 ? 0 : buffer_alignment - 1, &buffer);
  if (FAILED(hr)) {
    return nullptr;
  }

  BYTE* data = nullptr;
  DWORD max_length = 0;
  DWORD current_length = 0;
  hr = buffer->Lock(&data, &max_length, &current_length);
  if (FAILED(hr)) {
    return nullptr;
  }

  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t uv_size = static_cast<size_t>(width) * (height / 2);
  if (max_length < y_size + uv_size) {
    buffer->Unlock();
    RTC_LOG(LS_ERROR) << "The transform's input buffer is too small: "
                      << max_length << " for " << (y_size + uv_size);
    return nullptr;
  }

  // NV12, tightly packed: the media type carries no explicit stride, so the
  // default is the frame width.
  const int result = libyuv::I420ToNV12(
      i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
      i420->DataV(), i420->StrideV(), data, static_cast<int>(width),
      data + y_size, static_cast<int>(width), static_cast<int>(width),
      static_cast<int>(height));

  if (max_length > y_size + uv_size) {
    // The transform may hand back a buffer larger than the frame; leaving the
    // tail uninitialised would encode whatever was in that memory.
    memset(data + y_size + uv_size, 0, max_length - (y_size + uv_size));
  }

  buffer->Unlock();
  if (result != 0) {
    RTC_LOG(LS_ERROR) << "I420ToNV12 failed";
    return nullptr;
  }

  hr = buffer->SetCurrentLength(static_cast<DWORD>(y_size + uv_size));
  if (SUCCEEDED(hr)) {
    hr = sample->AddBuffer(buffer.Get());
  }
  if (SUCCEEDED(hr)) {
    hr = sample->SetSampleTime(time_hns);
  }
  if (SUCCEEDED(hr)) {
    hr = sample->SetSampleDuration(duration_hns);
  }
  if (FAILED(hr)) {
    return nullptr;
  }

  return sample;
}

HRESULT H264EncoderMFImpl::FeedSample(const ComPtr<IMFSample>& sample,
                                      const FrameMetadata& metadata) {
  const HRESULT hr =
      transform_->ProcessInput(input_stream_id_, sample.Get(), 0);
  if (SUCCEEDED(hr)) {
    // Bounded even though a transform that takes input normally produces
    // output: nothing in an encoder should be able to grow without a limit,
    // which is the whole reason this file was rewritten.
    while (in_flight_.size() >= kMaxInFlightFrames) {
      in_flight_.pop_front();
    }
    in_flight_.push_back(metadata);
  }
  return hr;
}

bool H264EncoderMFImpl::PopMetadata(LONGLONG time_hns, FrameMetadata* out) {
  // Encoders emit frames in input order, and drop rather than reorder, so
  // anything older than what came out is a frame the transform swallowed.
  while (!in_flight_.empty() && in_flight_.front().time_hns < time_hns) {
    in_flight_.pop_front();
  }
  if (in_flight_.empty()) {
    return false;
  }
  *out = in_flight_.front();
  in_flight_.pop_front();
  return true;
}

HRESULT H264EncoderMFImpl::ProcessOneOutput(std::vector<EncodedFrame>* out) {
  MFT_OUTPUT_DATA_BUFFER output_data = {};
  output_data.dwStreamID = output_stream_id_;

  ComPtr<IMFSample> allocated;
  if (!transform_allocates_output_) {
    HRESULT hr = MFCreateSample(&allocated);
    ComPtr<IMFMediaBuffer> buffer;
    if (SUCCEEDED(hr)) {
      hr = MFCreateMemoryBuffer(output_buffer_size_, &buffer);
    }
    if (SUCCEEDED(hr)) {
      hr = allocated->AddBuffer(buffer.Get());
    }
    if (FAILED(hr)) {
      return hr;
    }
    output_data.pSample = allocated.Get();
  }

  DWORD status = 0;
  HRESULT hr = transform_->ProcessOutput(0, 1, &output_data, &status);

  if (output_data.pEvents != nullptr) {
    output_data.pEvents->Release();
    output_data.pEvents = nullptr;
  }

  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    return hr;
  }

  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    // The transform wants to renegotiate. Take its first offer; the frame size
    // and bitrate we asked for are still on our own media type.
    ComPtr<IMFMediaType> media_type;
    if (SUCCEEDED(transform_->GetOutputAvailableType(output_stream_id_, 0,
                                                     &media_type))) {
      const HRESULT set = transform_->SetOutputType(output_stream_id_,
                                                    media_type.Get(), 0);
      if (FAILED(set)) {
        RTC_LOG(LS_WARNING) << "Couldn't accept the transform's output type: "
                            << HrToString(set);
      }
    }
    return hr;
  }

  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "ProcessOutput failed: " << HrToString(hr);
    return hr;
  }

  ComPtr<IMFSample> sample;
  if (transform_allocates_output_) {
    // The transform allocated it, so this reference is ours to release.
    sample.Attach(output_data.pSample);
  } else {
    sample = allocated;
  }
  if (sample == nullptr) {
    return E_UNEXPECTED;
  }

  LONGLONG sample_time = 0;
  sample->GetSampleTime(&sample_time);

  FrameMetadata metadata;
  if (!PopMetadata(sample_time, &metadata)) {
    // Nothing to attribute it to. Reporting it with invented timestamps would
    // corrupt the receiver's timing, so it is dropped.
    RTC_LOG(LS_WARNING) << "An encoded frame with no matching input";
    return S_OK;
  }

  ComPtr<IMFMediaBuffer> buffer;
  hr = sample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(hr)) {
    return hr;
  }

  BYTE* data = nullptr;
  DWORD max_length = 0;
  DWORD current_length = 0;
  hr = buffer->Lock(&data, &max_length, &current_length);
  if (FAILED(hr)) {
    return hr;
  }

  EncodedFrame frame;
  if (current_length > 0) {
    frame.image.SetEncodedData(
        EncodedImageBuffer::Create(data, current_length));
  }
  buffer->Unlock();

  if (current_length == 0) {
    return S_OK;
  }

  const bool keyframe =
      MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) !=
      FALSE;

  frame.image._frameType = keyframe ? VideoFrameType::kVideoFrameKey
                                    : VideoFrameType::kVideoFrameDelta;
  frame.image.SetRtpTimestamp(metadata.rtp_timestamp);
  frame.image.ntp_time_ms_ = metadata.ntp_time_ms;
  frame.image.capture_time_ms_ = metadata.capture_time_ms;
  frame.image._encodedWidth = metadata.width;
  frame.image._encodedHeight = metadata.height;

  // Bits 0-15 hold the frame QP. Reporting it saves WebRTC parsing the
  // bitstream for it, and is what its quality scaler adapts on.
  UINT64 qp = 0;
  if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_VideoEncodeQP, &qp))) {
    const UINT64 frame_qp = qp & 0xffffull;
    if (frame_qp <= kMaxH264Qp) {
      frame.image.qp_ = static_cast<int>(frame_qp);
    }
  }

  frame.codec_specific.codecType = kVideoCodecH264;
  frame.codec_specific.codecSpecific.H264.packetization_mode =
      H264PacketizationMode::NonInterleaved;
  frame.codec_specific.codecSpecific.H264.idr_frame = keyframe;
  frame.codec_specific.codecSpecific.H264.base_layer_sync = false;
  frame.codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;

  out->push_back(std::move(frame));
  return S_OK;
}

HRESULT H264EncoderMFImpl::DrainOutputs(std::vector<EncodedFrame>* out) {
  HRESULT hr = S_OK;
  for (int i = 0; i < kMaxOutputsPerDrain; ++i) {
    hr = ProcessOneOutput(out);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return S_OK;
    }
    if (FAILED(hr) && hr != MF_E_TRANSFORM_STREAM_CHANGE) {
      return hr;
    }
  }
  return hr;
}

void H264EncoderMFImpl::Deliver(std::vector<EncodedFrame>* frames) {
  if (frames->empty()) {
    return;
  }

  // Held across the callback deliberately: the alternative is reading the
  // pointer and calling it after the lock, which races Release(). Nothing in
  // here takes mutex_, so this cannot invert against the encode path.
  webrtc::MutexLock lock(&callback_mutex_);
  if (encoded_complete_callback_ == nullptr) {
    return;
  }
  for (const EncodedFrame& frame : *frames) {
    encoded_complete_callback_->OnEncodedImage(frame.image,
                                               &frame.codec_specific);
  }
}

void H264EncoderMFImpl::ReportDroppedFrame() {
  webrtc::MutexLock lock(&callback_mutex_);
  if (encoded_complete_callback_ != nullptr) {
    encoded_complete_callback_->OnDroppedFrame(
        EncodedImageCallback::DropReason::kDroppedByEncoder);
  }
}

void H264EncoderMFImpl::OnTransformEvent(IMFMediaEventGenerator* generator_raw,
                                         MediaEventType type,
                                         HRESULT status) {
  std::vector<EncodedFrame> encoded;
  ComPtr<IMFMediaEventGenerator> generator;

  {
    webrtc::MutexLock lock(&mutex_);
    // A transform released while one of its BeginGetEvent calls was still
    // outstanding delivers this event after another transform has taken over.
    // Acting on it would feed the new transform the old one's state.
    if (transform_ == nullptr || !streaming_ ||
        event_generator_.Get() != generator_raw) {
      return;
    }
    generator = event_generator_;

    if (FAILED(status)) {
      RTC_LOG(LS_ERROR) << "Transform event " << type
                        << " reported: " << HrToString(status);
      broken_ = true;
    } else {
      switch (type) {
        case METransformNeedInput: {
          if (pending_.empty()) {
            // Capped: a transform that keeps asking while nothing is being
            // captured must not build up credits it can spend all at once.
            needs_input_ =
                std::min<int>(needs_input_ + 1,
                              static_cast<int>(kMaxPendingInputs));
          } else {
            const ComPtr<IMFSample> sample = pending_.front();
            const FrameMetadata metadata = pending_metadata_.front();
            pending_.pop_front();
            pending_metadata_.pop_front();
            const HRESULT hr = FeedSample(sample, metadata);
            if (FAILED(hr)) {
              RTC_LOG(LS_WARNING) << "ProcessInput failed: " << HrToString(hr);
              broken_ = true;
            }
          }
          break;
        }
        case METransformHaveOutput: {
          const HRESULT hr = ProcessOneOutput(&encoded);
          if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT &&
              hr != MF_E_TRANSFORM_STREAM_CHANGE) {
            broken_ = true;
          }
          break;
        }
        case MEError:
          RTC_LOG(LS_ERROR) << "The encoder transform reported MEError";
          broken_ = true;
          break;
        default:
          break;
      }
    }
  }

  Deliver(&encoded);

  // Re-armed outside the lock, and outside the delivery, so that neither can be
  // re-entered by the next event before this one has finished with them.
  if (generator != nullptr && event_callback_ != nullptr) {
    const HRESULT hr =
        generator->BeginGetEvent(event_callback_.Get(), generator.Get());
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "BeginGetEvent failed: " << HrToString(hr);
    }
  }
}

int32_t H264EncoderMFImpl::Encode(const VideoFrame& frame,
                                  const std::vector<VideoFrameType>* types) {
  const UINT32 frame_width = RoundDownToEven(frame.width());
  const UINT32 frame_height = RoundDownToEven(frame.height());
  if (frame_width < 2 || frame_height < 2) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  bool keyframe_requested = false;
  if (types != nullptr) {
    for (const VideoFrameType type : *types) {
      if (type == VideoFrameType::kVideoFrameKey) {
        keyframe_requested = true;
        break;
      }
    }
  }

  ComPtr<IMFSample> sample;
  FrameMetadata metadata;
  DWORD buffer_size = 0;
  DWORD buffer_alignment = 0;
  LONGLONG duration_hns = 0;

  {
    webrtc::MutexLock lock(&mutex_);
    if (!inited_) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }

    // A transform that reported an error is rebuilt once, here, rather than
    // left to fail every frame for the rest of the call.
    if (broken_) {
      RTC_LOG(LS_WARNING) << "Rebuilding the encoder transform";
      ReleaseTransform();
      broken_ = false;
      if (InitTransform() != WEBRTC_VIDEO_CODEC_OK) {
        ReleaseTransform();
        inited_ = false;
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
    }

    if (frame_width != width_ || frame_height != height_) {
      if (!UpdateFrameSize(frame_width, frame_height)) {
        // Resizing in place failed, so fall back to a new transform. This is
        // the only path that builds one outside InitEncode, and it is bounded:
        // it happens once per failure, not once per frame.
        ReleaseTransform();
        width_ = frame_width;
        height_ = frame_height;
        if (InitTransform() != WEBRTC_VIDEO_CODEC_OK) {
          ReleaseTransform();
          inited_ = false;
          return WEBRTC_VIDEO_CODEC_ERROR;
        }
      }
    }

    if (keyframe_requested) {
      RequestKeyFrame();
    }

    metadata.time_hns = FrameTimeHns(frame);
    metadata.rtp_timestamp = frame.timestamp();
    metadata.ntp_time_ms = frame.ntp_time_ms();
    metadata.capture_time_ms = frame.render_time_ms();
    metadata.width = width_;
    metadata.height = height_;

    buffer_size = input_buffer_size_;
    buffer_alignment = input_buffer_alignment_;
    duration_hns = kOneSecondInHns / std::max<UINT32>(fps_, 1);
  }

  // Outside the lock: this is a full frame conversion and copy, and holding the
  // lock across it would stall the event thread behind it.
  sample = CreateInputSample(frame, frame_width, frame_height, buffer_size,
                             buffer_alignment, metadata.time_hns, duration_hns);
  if (sample == nullptr) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  std::vector<EncodedFrame> encoded;
  bool dropped = false;

  {
    webrtc::MutexLock lock(&mutex_);
    if (!inited_ || transform_ == nullptr || !streaming_) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }

    if (is_async_) {
      if (needs_input_ > 0) {
        --needs_input_;
        const HRESULT hr = FeedSample(sample, metadata);
        if (FAILED(hr)) {
          RTC_LOG(LS_WARNING) << "ProcessInput failed: " << HrToString(hr);
          broken_ = true;
          return WEBRTC_VIDEO_CODEC_ERROR;
        }
      } else if (pending_.size() < kMaxPendingInputs) {
        pending_.push_back(sample);
        pending_metadata_.push_back(metadata);
      } else {
        // The hardware has stopped asking for input. Dropping here is what
        // keeps the encoder queue from turning into a frame buffer.
        dropped = true;
      }
    } else {
      HRESULT hr = FeedSample(sample, metadata);
      if (hr == MF_E_NOTACCEPTING) {
        // A synchronous transform refuses input until its output is taken.
        DrainOutputs(&encoded);
        hr = FeedSample(sample, metadata);
      }
      if (FAILED(hr)) {
        RTC_LOG(LS_WARNING) << "ProcessInput failed: " << HrToString(hr);
        broken_ = true;
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
      const HRESULT drain = DrainOutputs(&encoded);
      if (FAILED(drain) && drain != MF_E_TRANSFORM_STREAM_CHANGE) {
        broken_ = true;
      }
    }
  }

  Deliver(&encoded);
  if (dropped) {
    ReportDroppedFrame();
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

VideoEncoder::EncoderInfo H264EncoderMFImpl::GetEncoderInfo() const {
  webrtc::MutexLock lock(&mutex_);

  EncoderInfo info;
  info.supports_native_handle = false;
  info.implementation_name = implementation_name_.empty()
                                 ? "MediaFoundation"
                                 : "MediaFoundation: " + implementation_name_;
  info.scaling_settings =
      VideoEncoder::ScalingSettings(kLowH264QpThreshold, kHighH264QpThreshold);
  // Answered from the transform that was actually activated, rather than
  // asserted. The previous implementation could not tell, because the sink
  // writer chose the transform for it.
  info.is_hardware_accelerated = is_hardware_;
  info.supports_simulcast = false;
  return info;
}

}  // namespace webrtc
