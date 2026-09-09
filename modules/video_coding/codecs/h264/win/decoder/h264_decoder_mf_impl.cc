/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/codecs/h264/win/decoder/h264_decoder_mf_impl.h"

#include <Windows.h>
#include <codecapi.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

#include "../utils/utils.h"
#include "common_video/include/video_frame_buffer.h"
#include "libyuv/convert.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/string_utils.h"

using Microsoft::WRL::ComPtr;

namespace webrtc {
namespace {

constexpr int kMaxReadbackFailures = 3;

std::string HrToString(HRESULT hr) {
  return "0x" + rtc::ToHex(hr);
}

HRESULT ConfigureOutputMediaType(const ComPtr<IMFTransform>& decoder,
                                 GUID media_type,
                                 bool* type_found) {
  *type_found = false;

  for (DWORD type = 0;; ++type) {
    ComPtr<IMFMediaType> output_media;
    HRESULT hr = decoder->GetOutputAvailableType(0, type, &output_media);
    if (hr == MF_E_NO_MORE_TYPES) {
      return S_OK;
    }
    if (FAILED(hr)) {
      return hr;
    }

    GUID current = GUID_NULL;
    hr = output_media->GetGUID(MF_MT_SUBTYPE, &current);
    if (FAILED(hr)) {
      return hr;
    }

    if (current == media_type) {
      hr = decoder->SetOutputType(0, output_media.Get(), 0);
      if (SUCCEEDED(hr)) {
        *type_found = true;
      }
      return hr;
    }
  }
}

HRESULT CreateInputMediaType(IMFMediaType** pp_input_media,
                             absl::optional<UINT32> img_width,
                             absl::optional<UINT32> img_height,
                             absl::optional<UINT32> frame_rate) {
  HRESULT hr = MFCreateMediaType(pp_input_media);
  if (FAILED(hr)) {
    return hr;
  }

  IMFMediaType* input_media = *pp_input_media;
  ON_SUCCEEDED(input_media->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
  ON_SUCCEEDED(input_media->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
  ON_SUCCEEDED(MFSetAttributeRatio(input_media, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
  ON_SUCCEEDED(input_media->SetUINT32(
      MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive));

  if (frame_rate.has_value()) {
    ON_SUCCEEDED(
        MFSetAttributeRatio(input_media, MF_MT_FRAME_RATE, frame_rate.value(), 1));
  }

  if (img_width.has_value() && img_height.has_value()) {
    ON_SUCCEEDED(MFSetAttributeSize(input_media, MF_MT_FRAME_SIZE,
                                    img_width.value(), img_height.value()));
  }

  return hr;
}

// Workaround for a Media Foundation H.264 decoder bug: the output status is
// never set, even when a frame is ready. Always reporting ready costs an extra
// ProcessOutput attempt per decode, which is why the output sample is reused.
HRESULT GetOutputStatus(const ComPtr<IMFTransform>& decoder,
                        DWORD* output_status) {
  const HRESULT hr = decoder->GetOutputStatus(output_status);
  *output_status = MFT_OUTPUT_STATUS_SAMPLE_READY;
  return hr;
}

}  // namespace

H264DecoderMFImpl::H264DecoderMFImpl()
    : buffer_pool_(false, 300), /* max_number_of_buffers */
      width_(absl::nullopt),
      height_(absl::nullopt),
      implementation_name_("H264_MediaFoundation"),
      decode_complete_callback_(nullptr) {
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  mf_started_ = SUCCEEDED(hr);
  if (!mf_started_) {
    RTC_LOG(LS_ERROR) << "MFStartup failed: " << HrToString(hr);
  }
}

H264DecoderMFImpl::~H264DecoderMFImpl() {
  Release();
  if (mf_started_) {
    MFShutdown();
  }
}

bool H264DecoderMFImpl::CreateD3DManager() {
  // BGRA support is what lets the same device back Direct2D interop later;
  // video support is what the decoder needs.
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
               D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
      D3D_FEATURE_LEVEL_9_1};

  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 flags, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &d3d_device_, &level,
                                 &d3d_context_);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "No D3D11 device for decoding: " << HrToString(hr);
    return false;
  }

  // The transform uses the device from its own threads, and so do we when we
  // read a frame back. Without this the driver is free to corrupt state.
  ComPtr<ID3D11Multithread> multithread;
  if (SUCCEEDED(d3d_device_.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  } else {
    RTC_LOG(LS_WARNING) << "No ID3D11Multithread; not using D3D decoding";
    ReleaseD3D();
    return false;
  }

  hr = MFCreateDXGIDeviceManager(&dxgi_reset_token_, &dxgi_manager_);
  if (SUCCEEDED(hr)) {
    hr = dxgi_manager_->ResetDevice(d3d_device_.Get(), dxgi_reset_token_);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "No DXGI device manager: " << HrToString(hr);
    ReleaseD3D();
    return false;
  }

  return true;
}

void H264DecoderMFImpl::ReleaseD3D() {
  staging_texture_.Reset();
  staging_width_ = 0;
  staging_height_ = 0;
  dxgi_manager_.Reset();
  d3d_context_.Reset();
  d3d_device_.Reset();
  using_d3d_ = false;
}

bool H264DecoderMFImpl::ActivateTransform(bool use_d3d) {
  MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, MFVideoFormat_H264};
  MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_NV12};

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  // Deliberately not MFT_ENUM_FLAG_HARDWARE. A vendor's hardware decoder is
  // asynchronous, which this class does not drive, and the one synchronous
  // transform every Windows install has -- Microsoft's -- decodes on the GPU
  // anyway once it is given the device manager below. Enumerating hardware
  // would only offer rarer, less tested transforms ahead of it.
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                             MFT_ENUM_FLAG_SORTANDFILTER,
                         &input_info, &output_info, &activates, &count);
  if (FAILED(hr) || count == 0) {
    RTC_LOG(LS_ERROR) << "No H.264 decoder transform: " << HrToString(hr);
    if (SUCCEEDED(hr)) {
      CoTaskMemFree(activates);
    }
    return false;
  }

  for (UINT32 i = 0; i < count; ++i) {
    ComPtr<IMFActivate> activate(activates[i]);

    ComPtr<IMFTransform> transform;
    hr = activate->ActivateObject(IID_PPV_ARGS(&transform));
    if (FAILED(hr) || transform == nullptr) {
      activate->ShutdownObject();
      continue;
    }

    ComPtr<IMFAttributes> attributes;
    UINT32 is_async = FALSE;
    UINT32 is_d3d_aware = FALSE;
    if (SUCCEEDED(transform->GetAttributes(&attributes)) &&
        attributes != nullptr) {
      attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
      attributes->GetUINT32(MF_SA_D3D11_AWARE, &is_d3d_aware);
    }

    // Asynchronous transforms are driven by transform events rather than by
    // ProcessOutput, which this decoder does not do. On every Windows install
    // the synchronous Microsoft decoder is present and DXVA-capable, so
    // skipping them costs nothing today.
    if (is_async) {
      activate->ShutdownObject();
      continue;
    }

    if (attributes != nullptr) {
      attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
      attributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE);
    }

    bool d3d_attached = false;
    if (use_d3d && is_d3d_aware && dxgi_manager_ != nullptr) {
      // This is what actually turns DXVA on. Without it the Microsoft decoder
      // decodes in software however its acceleration attribute is set, which is
      // what every previous release of this app did.
      hr = transform->ProcessMessage(
          MFT_MESSAGE_SET_D3D_MANAGER,
          reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get()));
      d3d_attached = SUCCEEDED(hr);
      if (!d3d_attached) {
        RTC_LOG(LS_WARNING) << "The decoder refused the D3D manager: "
                            << HrToString(hr);
      }
    }

    ComPtr<IMFMediaType> input_media;
    hr = CreateInputMediaType(input_media.GetAddressOf(), width_, height_,
                              absl::nullopt);
    if (SUCCEEDED(hr)) {
      hr = transform->SetInputType(0, input_media.Get(), 0);
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "Couldn't set the decoder input type: "
                          << HrToString(hr);
      activate->ShutdownObject();
      continue;
    }

    bool suitable_type_found = false;
    hr = ConfigureOutputMediaType(transform, MFVideoFormat_NV12,
                                  &suitable_type_found);
    if (FAILED(hr) || !suitable_type_found) {
      RTC_LOG(LS_WARNING) << "The decoder has no NV12 output type";
      activate->ShutdownObject();
      continue;
    }

    DWORD status = 0;
    hr = transform->GetInputStatus(0, &status);
    if (SUCCEEDED(hr) && status != MFT_INPUT_STATUS_ACCEPT_DATA) {
      RTC_LOG(LS_WARNING) << "The decoder is not accepting data";
      activate->ShutdownObject();
      continue;
    }

    MFT_OUTPUT_STREAM_INFO output_info_stream = {};
    if (SUCCEEDED(transform->GetOutputStreamInfo(0, &output_info_stream))) {
      transform_allocates_output_ =
          (output_info_stream.dwFlags &
           (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
            MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
      output_sample_size_ = output_info_stream.cbSize;
    } else {
      transform_allocates_output_ = false;
      output_sample_size_ = 0;
    }

    transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    LPWSTR friendly_name = nullptr;
    UINT32 friendly_name_length = 0;
    std::string name = "Unknown MFT";
    if (SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                               &friendly_name,
                                               &friendly_name_length))) {
      name = rtc::ToUtf8(friendly_name, friendly_name_length);
      CoTaskMemFree(friendly_name);
    }

    decoder_ = transform;
    activate_ = activate;
    using_d3d_ = d3d_attached;
    implementation_name_ =
        "H264_MediaFoundation" + std::string(d3d_attached ? " (D3D11)" : "");

    RTC_LOG(LS_INFO) << "H.264 decoder: " << name << ", "
                     << (d3d_attached ? "D3D11" : "system memory");

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
  return false;
}

bool H264DecoderMFImpl::InitTransform(bool use_d3d) {
  if (use_d3d && dxgi_manager_ == nullptr) {
    // Not fatal: a machine without a usable D3D11 device decodes in software,
    // which is what it did before.
    CreateD3DManager();
  }

  if (!ActivateTransform(use_d3d)) {
    return false;
  }

  if (!using_d3d_) {
    ReleaseD3D();
  }

  readback_failures_ = 0;
  return true;
}

void H264DecoderMFImpl::ReleaseTransform() {
  if (decoder_ != nullptr) {
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    decoder_.Reset();
  }
  if (activate_ != nullptr) {
    activate_->ShutdownObject();
    activate_.Reset();
  }

  output_sample_.Reset();
  output_sample_size_ = 0;
  transform_allocates_output_ = false;
  staging_texture_.Reset();
  staging_width_ = 0;
  staging_height_ = 0;
  using_d3d_ = false;
}

bool H264DecoderMFImpl::Configure(const Settings& settings) {
  if (!mf_started_) {
    return false;
  }

  const auto resolution = settings.max_render_resolution();
  width_ = resolution.Valid() ? absl::optional<UINT32>(resolution.Width())
                              : absl::nullopt;
  height_ = resolution.Valid() ? absl::optional<UINT32>(resolution.Height())
                               : absl::nullopt;

  ReleaseTransform();
  if (!InitTransform(/*use_d3d=*/true)) {
    // One retry without D3D: a transform can accept the manager and then fail
    // to negotiate, and software decoding is better than no video.
    ReleaseD3D();
    if (!InitTransform(/*use_d3d=*/false)) {
      RTC_LOG(LS_ERROR) << "Could not initialise any H.264 decoder";
      return false;
    }
  }

  inited_ = true;
  return true;
}

HRESULT H264DecoderMFImpl::TextureToI420(IMFDXGIBuffer* dxgi_buffer,
                                         UINT32 width,
                                         UINT32 height,
                                         I420Buffer* buffer) {
  ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = dxgi_buffer->GetResource(IID_PPV_ARGS(&texture));
  if (FAILED(hr)) {
    return hr;
  }

  UINT subresource = 0;
  dxgi_buffer->GetSubresourceIndex(&subresource);

  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);

  // Only NV12 is negotiated, so anything else means the transform switched
  // formats behind us and the readback would misinterpret the bytes.
  if (desc.Format != DXGI_FORMAT_NV12) {
    RTC_LOG(LS_WARNING) << "Unexpected decoder texture format: " << desc.Format;
    return MF_E_INVALIDMEDIATYPE;
  }
  if (width > desc.Width || height > desc.Height) {
    RTC_LOG(LS_WARNING) << "The decoder texture is smaller than the frame";
    return MF_E_INVALIDMEDIATYPE;
  }

  if (staging_texture_ == nullptr || staging_width_ != desc.Width ||
      staging_height_ != desc.Height) {
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    staging.ArraySize = 1;
    staging.MipLevels = 1;
    staging.SampleDesc.Count = 1;
    staging.SampleDesc.Quality = 0;

    staging_texture_.Reset();
    hr = d3d_device_->CreateTexture2D(&staging, nullptr, &staging_texture_);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "No staging texture: " << HrToString(hr);
      return hr;
    }
    staging_width_ = desc.Width;
    staging_height_ = desc.Height;
  }

  d3d_context_->CopySubresourceRegion(staging_texture_.Get(), 0, 0, 0, 0,
                                      texture.Get(), subresource, nullptr);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = d3d_context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Couldn't map the staging texture: "
                        << HrToString(hr);
    return hr;
  }

  // NV12: the UV plane follows the full height of the Y plane at the same
  // pitch, which is how a mapped NV12 texture is laid out.
  const uint8_t* src_y = static_cast<const uint8_t*>(mapped.pData);
  const uint8_t* src_uv =
      src_y + static_cast<size_t>(mapped.RowPitch) * desc.Height;

  const int result = libyuv::NV12ToI420(
      src_y, static_cast<int>(mapped.RowPitch), src_uv,
      static_cast<int>(mapped.RowPitch), buffer->MutableDataY(),
      buffer->StrideY(), buffer->MutableDataU(), buffer->StrideU(),
      buffer->MutableDataV(), buffer->StrideV(), static_cast<int>(width),
      static_cast<int>(height));

  d3d_context_->Unmap(staging_texture_.Get(), 0);
  return result == 0 ? S_OK : E_FAIL;
}

HRESULT H264DecoderMFImpl::SampleToI420(IMFSample* sample,
                                        UINT32 width,
                                        UINT32 height,
                                        UINT32 buffer_width,
                                        UINT32 buffer_height,
                                        I420Buffer* buffer) {
  ComPtr<IMFMediaBuffer> media_buffer;
  HRESULT hr = sample->GetBufferByIndex(0, &media_buffer);
  if (FAILED(hr)) {
    return hr;
  }

  ComPtr<IMFDXGIBuffer> dxgi_buffer;
  if (using_d3d_ && SUCCEEDED(media_buffer.As(&dxgi_buffer))) {
    hr = TextureToI420(dxgi_buffer.Get(), width, height, buffer);
    if (SUCCEEDED(hr)) {
      readback_failures_ = 0;
    } else {
      ++readback_failures_;
    }
    return hr;
  }

  ComPtr<IMFMediaBuffer> contiguous;
  hr = sample->ConvertToContiguousBuffer(&contiguous);
  if (FAILED(hr)) {
    return hr;
  }

  BYTE* data = nullptr;
  DWORD max_length = 0;
  DWORD current_length = 0;
  hr = contiguous->Lock(&data, &max_length, &current_length);
  if (FAILED(hr)) {
    return hr;
  }

  // The plane sizes come from the transform describing a stream a remote peer
  // chose, so they are checked against what the buffer actually holds before
  // anything indexes into it. Computed in 64 bit: the product of two UINT32s is
  // not an int.
  const uint64_t y_size = uint64_t{buffer_width} * buffer_height;
  const uint64_t uv_size = uint64_t{buffer_width} * ((buffer_height + 1) / 2);
  if (current_length < y_size + uv_size) {
    contiguous->Unlock();
    RTC_LOG(LS_ERROR) << "Decode failure: the output buffer holds "
                      << current_length << " bytes, " << (y_size + uv_size)
                      << " needed for " << buffer_width << "x" << buffer_height
                      << " NV12.";
    return MF_E_BUFFERTOOSMALL;
  }

  const int result = libyuv::NV12ToI420(
      data, static_cast<int>(buffer_width), data + y_size,
      static_cast<int>(buffer_width), buffer->MutableDataY(),
      buffer->StrideY(), buffer->MutableDataU(), buffer->StrideU(),
      buffer->MutableDataV(), buffer->StrideV(), static_cast<int>(width),
      static_cast<int>(height));

  contiguous->Unlock();
  return result == 0 ? S_OK : E_FAIL;
}

HRESULT H264DecoderMFImpl::FlushFrames(uint32_t rtp_timestamp,
                                       uint64_t ntp_time_ms) {
  HRESULT hr = S_OK;
  DWORD output_status = 0;

  while (SUCCEEDED(hr = GetOutputStatus(decoder_, &output_status)) &&
         output_status == MFT_OUTPUT_STATUS_SAMPLE_READY) {
    MFT_OUTPUT_DATA_BUFFER output_data = {};
    output_data.dwStreamID = 0;

    if (!transform_allocates_output_) {
      // Allocated once and reused. The previous implementation built a sample
      // and a full frame buffer for every attempt, and it attempts on every
      // decode whether or not a frame is ready.
      if (output_sample_ == nullptr) {
        ComPtr<IMFMediaBuffer> out_buffer;
        hr = MFCreateMemoryBuffer(output_sample_size_, &out_buffer);
        if (SUCCEEDED(hr)) {
          hr = MFCreateSample(&output_sample_);
        }
        if (SUCCEEDED(hr)) {
          hr = output_sample_->AddBuffer(out_buffer.Get());
        }
        if (FAILED(hr)) {
          output_sample_.Reset();
          RTC_LOG(LS_ERROR) << "Decode failure: no output sample: "
                            << HrToString(hr);
          return hr;
        }
      }

      ComPtr<IMFMediaBuffer> out_buffer;
      if (SUCCEEDED(output_sample_->GetBufferByIndex(0, &out_buffer))) {
        out_buffer->SetCurrentLength(0);
      }
      output_data.pSample = output_sample_.Get();
    }

    DWORD status = 0;
    hr = decoder_->ProcessOutput(0, 1, &output_data, &status);

    if (output_data.pEvents != nullptr) {
      output_data.pEvents->Release();
      output_data.pEvents = nullptr;
    }

    if (FAILED(hr)) {
      // MF_E_TRANSFORM_NEED_MORE_INPUT and MF_E_TRANSFORM_STREAM_CHANGE are
      // both expected and handled by the caller.
      return hr;
    }

    ComPtr<IMFSample> sample;
    if (transform_allocates_output_) {
      sample.Attach(output_data.pSample);
    } else {
      sample = output_sample_;
    }
    if (sample == nullptr) {
      return E_UNEXPECTED;
    }

    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 buffer_width = 0;
    UINT32 buffer_height = 0;
    {
      ComPtr<IMFMediaType> output_type;
      hr = decoder_->GetOutputCurrentType(0, output_type.GetAddressOf());
      if (SUCCEEDED(hr)) {
        // The padded frame size, which is what the buffer is laid out for.
        hr = MFGetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE,
                                &buffer_width, &buffer_height);
      }
      if (FAILED(hr)) {
        RTC_LOG(LS_ERROR) << "Decode failure: no output frame size";
        return hr;
      }

      // The visible area, when the transform reports one. Missing means the
      // frame is not padded.
      MFVideoArea video_area = {};
      if (SUCCEEDED(output_type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                         reinterpret_cast<UINT8*>(&video_area),
                                         sizeof(video_area), nullptr))) {
        width = video_area.Area.cx;
        height = video_area.Area.cy;
      } else {
        width = buffer_width;
        height = buffer_height;
      }

      width_.emplace(width);
      height_.emplace(height);
    }

    // Everything below indexes with these, and they come from the transform
    // describing a stream a remote peer chose. The visible area has to sit
    // inside the padded frame.
    if (buffer_width == 0 || buffer_height == 0 || width == 0 || height == 0 ||
        width > buffer_width || height > buffer_height) {
      RTC_LOG(LS_ERROR) << "Decode failure: bad output frame size, visible "
                        << width << "x" << height << " in padded "
                        << buffer_width << "x" << buffer_height;
      return MF_E_INVALIDMEDIATYPE;
    }

    rtc::scoped_refptr<I420Buffer> buffer =
        buffer_pool_.CreateI420Buffer(width, height);
    if (buffer == nullptr) {
      RTC_LOG(LS_WARNING) << "Decode warning: too many frames. Dropping frame.";
      return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
    }

    hr = SampleToI420(sample.Get(), width, height, buffer_width, buffer_height,
                      buffer.get());
    if (FAILED(hr)) {
      return hr;
    }

    // The transform may interpolate its own sample times, so the RTP timestamp
    // of the frame that produced this one is used instead.
    VideoFrame decoded_frame(buffer, rtp_timestamp, 0, kVideoRotation_0);
    decoded_frame.set_ntp_time_ms(ntp_time_ms);

    if (decode_complete_callback_ != nullptr) {
      decode_complete_callback_->Decoded(decoded_frame, absl::nullopt,
                                         absl::nullopt);
    }
  }

  return hr;
}

HRESULT H264DecoderMFImpl::EnqueueFrame(const EncodedImage& input_image,
                                        bool missing_frames) {
  HRESULT hr = S_OK;

  ComPtr<IMFMediaBuffer> in_buffer;
  ON_SUCCEEDED(MFCreateMemoryBuffer(input_image.size(), &in_buffer));
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR)
        << "Decode failure: input image memory buffer creation failed.";
    return hr;
  }

  DWORD max_len = 0;
  DWORD cur_len = 0;
  BYTE* data = nullptr;
  ON_SUCCEEDED(in_buffer->Lock(&data, &max_len, &cur_len));
  if (FAILED(hr)) {
    return hr;
  }

  memcpy(data, input_image.data(), input_image.size());

  ON_SUCCEEDED(in_buffer->Unlock());
  if (FAILED(hr)) {
    return hr;
  }

  ON_SUCCEEDED(in_buffer->SetCurrentLength(input_image.size()));
  if (FAILED(hr)) {
    return hr;
  }

  ComPtr<IMFSample> in_sample;
  ON_SUCCEEDED(MFCreateSample(&in_sample));
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Decode failure: input in_sample creation failed.";
    return hr;
  }

  ON_SUCCEEDED(in_sample->AddBuffer(in_buffer.Get()));
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR)
        << "Decode failure: failed to add buffer to input in_sample.";
    return hr;
  }

  int64_t sample_time_ms;
  if (first_frame_rtp_ == 0) {
    first_frame_rtp_ = input_image.RtpTimestamp();
    sample_time_ms = 0;
  } else {
    // Convert from 90 kHz, rounding to the nearest millisecond.
    sample_time_ms = static_cast<int64_t>(
        (static_cast<int64_t>(input_image.RtpTimestamp()) - first_frame_rtp_) /
            90.0 +
        0.5);
  }

  ON_SUCCEEDED(in_sample->SetSampleTime(sample_time_ms * 10000));
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR)
        << "Decode failure: failed to set in_sample time on input in_sample.";
    return hr;
  }

  ComPtr<IMFAttributes> sample_attrs;
  ON_SUCCEEDED(in_sample.As(&sample_attrs));

  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING)
        << "Decode warning: failed to set image attributes for frame.";
    hr = S_OK;
  } else {
    if (input_image._frameType == VideoFrameType::kVideoFrameKey) {
      ON_SUCCEEDED(sample_attrs->SetUINT32(MFSampleExtension_CleanPoint, TRUE));
      hr = S_OK;
    }

    if (missing_frames) {
      ON_SUCCEEDED(
          sample_attrs->SetUINT32(MFSampleExtension_Discontinuity, TRUE));
      hr = S_OK;
    }
  }

  ON_SUCCEEDED(decoder_->ProcessInput(0, in_sample.Get(), 0));
  return hr;
}

int H264DecoderMFImpl::Decode(const EncodedImage& input_image,
                              bool missing_frames,
                              int64_t /*render_time_ms*/) {
  HRESULT hr = S_OK;

  if (!inited_ || decoder_ == nullptr) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  if (decode_complete_callback_ == nullptr) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  if (input_image.data() == nullptr && input_image.size() > 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  // The D3D path has failed often enough to call it broken on this machine.
  // Rebuilding in software keeps the call alive rather than dropping every
  // frame from here on.
  if (readback_failures_ >= kMaxReadbackFailures) {
    RTC_LOG(LS_WARNING) << "Falling back to software H.264 decoding";
    ReleaseTransform();
    ReleaseD3D();
    if (!InitTransform(/*use_d3d=*/false)) {
      inited_ = false;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    require_keyframe_ = true;
  }

  if (require_keyframe_) {
    if (input_image._frameType != VideoFrameType::kVideoFrameKey) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    require_keyframe_ = false;
  }

  ON_SUCCEEDED(EnqueueFrame(input_image, missing_frames));
  if (hr == MF_E_NOTACCEPTING) {
    // For robustness; it should not happen, since the last loop flushed.
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);

    if (input_image._frameType == VideoFrameType::kVideoFrameKey) {
      hr = S_OK;
      ON_SUCCEEDED(EnqueueFrame(input_image, missing_frames));
    } else {
      require_keyframe_ = true;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }

  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  hr = FlushFrames(input_image.RtpTimestamp(), input_image.ntp_time_ms_);

  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    bool suitable_type_found = false;
    hr = ConfigureOutputMediaType(decoder_, MFVideoFormat_NV12,
                                  &suitable_type_found);

    if (FAILED(hr) || !suitable_type_found) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    // The output size may have changed with the type, and the sample sized for
    // the old one no longer fits.
    output_sample_.Reset();
    MFT_OUTPUT_STREAM_INFO output_info = {};
    if (SUCCEEDED(decoder_->GetOutputStreamInfo(0, &output_info))) {
      output_sample_size_ = output_info.cbSize;
    }

    width_.reset();
    height_.reset();

    hr = FlushFrames(input_image.RtpTimestamp(), input_image.ntp_time_ms_);
  }

  if (SUCCEEDED(hr) || hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    return WEBRTC_VIDEO_CODEC_OK;
  }

  return WEBRTC_VIDEO_CODEC_ERROR;
}

int H264DecoderMFImpl::RegisterDecodeCompleteCallback(
    DecodedImageCallback* callback) {
  webrtc::MutexLock lock(&crit_);
  decode_complete_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int H264DecoderMFImpl::Release() {
  inited_ = false;
  require_keyframe_ = true;
  first_frame_rtp_ = 0;

  buffer_pool_.Release();
  ReleaseTransform();
  ReleaseD3D();

  return WEBRTC_VIDEO_CODEC_OK;
}

const char* H264DecoderMFImpl::ImplementationName() const {
  return implementation_name_.c_str();
}

}  // namespace webrtc
