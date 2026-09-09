/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_H264_WIN_ENCODER_H264_DECODER_MF_IMPL_H_
#define MODULES_VIDEO_CODING_CODECS_H264_WIN_ENCODER_H264_DECODER_MF_IMPL_H_

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <string>

#include "api/video_codecs/video_decoder.h"
#include "common_video/include/video_frame_buffer_pool.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "rtc_base/synchronization/mutex.h"

namespace webrtc {

// H.264 decoder on a Media Foundation transform.
//
// The transform is given a D3D11 device manager, which is what makes it decode
// on the GPU: Microsoft's decoder is DXVA-capable but stays in software until
// one is set, and setting one was missing here, so every call decoded on the
// CPU. Frames are read back and converted to I420 because that is what the rest
// of the pipeline takes; handing the texture through to the renderer is the
// next step, and the one that removes the readback.
//
// Everything falls back: no device, no manager, a transform that refuses one, or
// repeated readback failures all end with a software transform producing system
// memory, which is exactly what shipped before.
class H264DecoderMFImpl : public H264Decoder {
 public:
  H264DecoderMFImpl();

  ~H264DecoderMFImpl() override;

  bool Configure(const Settings& settings) override;

  int Decode(const EncodedImage& input_image,
             bool missing_frames,
             int64_t /*render_time_ms*/) override;

  int RegisterDecodeCompleteCallback(DecodedImageCallback* callback) override;

  int Release() override;

  const char* ImplementationName() const override;

 private:
  // `use_d3d` false builds the software path directly; it is what the fallback
  // re-initialises with.
  bool InitTransform(bool use_d3d);
  void ReleaseTransform();
  bool ActivateTransform(bool use_d3d);
  bool CreateD3DManager();
  void ReleaseD3D();

  HRESULT FlushFrames(uint32_t timestamp, uint64_t ntp_time_ms);
  HRESULT EnqueueFrame(const EncodedImage& input_image, bool missing_frames);

  // Reads a decoded frame out of `sample` into `buffer`, from a D3D11 texture
  // or from system memory depending on what the transform produced.
  HRESULT SampleToI420(IMFSample* sample,
                       UINT32 width,
                       UINT32 height,
                       UINT32 buffer_width,
                       UINT32 buffer_height,
                       I420Buffer* buffer);
  HRESULT TextureToI420(IMFDXGIBuffer* dxgi_buffer,
                        UINT32 width,
                        UINT32 height,
                        I420Buffer* buffer);

  Microsoft::WRL::ComPtr<IMFTransform> decoder_;
  Microsoft::WRL::ComPtr<IMFActivate> activate_;
  // Reused across ProcessOutput calls. The previous implementation allocated a
  // full frame sample and buffer per attempt, and it attempts on every decode.
  Microsoft::WRL::ComPtr<IMFSample> output_sample_;
  DWORD output_sample_size_ = 0;
  bool transform_allocates_output_ = false;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgi_manager_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;
  UINT dxgi_reset_token_ = 0;
  UINT staging_width_ = 0;
  UINT staging_height_ = 0;
  bool using_d3d_ = false;
  // Three readbacks in a row failing means the D3D path is not working on this
  // machine; the decoder re-initialises in software rather than dropping every
  // frame for the rest of the call.
  int readback_failures_ = 0;

  VideoFrameBufferPool buffer_pool_;

  // MFStartup/MFShutdown are refcounted per process, so shutting down after a
  // failed startup decrements someone else's reference and tears Media
  // Foundation down under whichever encoder or decoder is still using it.
  bool mf_started_ = false;
  bool inited_ = false;
  bool require_keyframe_ = true;
  uint32_t first_frame_rtp_ = 0;
  absl::optional<uint32_t> width_;
  absl::optional<uint32_t> height_;
  std::string implementation_name_;
  webrtc::Mutex crit_;
  DecodedImageCallback* decode_complete_callback_;
};

}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_CODECS_H264_WIN_ENCODER_H264_DECODER_MF_IMPL_H_
