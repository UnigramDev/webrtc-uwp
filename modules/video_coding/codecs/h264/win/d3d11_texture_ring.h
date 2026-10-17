/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_H264_WIN_D3D11_TEXTURE_RING_H_
#define MODULES_VIDEO_CODING_CODECS_H264_WIN_D3D11_TEXTURE_RING_H_

#include <d3d11_4.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <map>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#include "rtc_base/ref_count.h"
#include "rtc_base/synchronization/mutex.h"

namespace webrtc {

// The shareable NV12 textures one decoder hands out as D3D11TextureBuffers.
//
// The decoder's own output belongs to the transform's sample pool, and holding
// it stalls the transform, so each frame is copied on the GPU into a free slot
// here instead. A slot is busy for as long as a buffer wrapping it lives; with
// none free the decoder reads the frame back, as it always did.
//
// Slots are taken round robin, so a slot is reused as late as possible, and
// the copy into a reused slot waits on the GPU for every reader that reported
// a fence for it: a buffer being released only means the CPU is done with it.
class D3D11TextureRing : public rtc::RefCountInterface {
 public:
  static constexpr int kSlots = 6;

  // Null when the device cannot share NV12 textures or fences (D3D11.4 and
  // WDDM 2.0 are needed), and texture output is then off for the decoder.
  static rtc::scoped_refptr<D3D11TextureRing> Create(ID3D11Device* device,
                                                     int texture_width,
                                                     int texture_height);

  ~D3D11TextureRing() override;

  int texture_width() const { return texture_width_; }
  int texture_height() const { return texture_height_; }

  // Copies `subresource` of `source` into a free slot. Null when every slot
  // is in use.
  rtc::scoped_refptr<VideoFrameBuffer> Wrap(ID3D11Texture2D* source,
                                            UINT subresource,
                                            int width,
                                            int height,
                                            bool bt709,
                                            bool full_range);

 protected:
  D3D11TextureRing() = default;

 private:
  class Buffer;
  friend class Buffer;

  struct ReadFence {
    ID3D11Fence* fence;
    uint64_t value;
  };

  struct Slot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HANDLE handle = nullptr;
    std::atomic<bool> busy{false};
    // Guarded by read_fences_lock_.
    std::vector<ReadFence> read_fences;
  };

  bool Initialize(ID3D11Device* device, int texture_width, int texture_height);
  void ReleaseSlot(int slot);
  void AddReadFence(int slot, uint64_t reader_id, HANDLE fence, uint64_t value);
  rtc::scoped_refptr<I420BufferInterface> ReadBack(int slot,
                                                   int width,
                                                   int height);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11Device5> device5_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
  Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
  HANDLE fence_handle_ = nullptr;
  uint64_t fence_value_ = 0;

  std::array<Slot, kSlots> slots_;
  int next_slot_ = 0;

  uint64_t ring_id_ = 0;

  // This ring's share of D3D11TextureBuffer::counters(). The first of each is
  // logged, and all three when the ring goes: a readback is what silently
  // undoes the whole texture path, and nothing else would show it.
  std::atomic<uint64_t> textures_{0};
  std::atomic<uint64_t> ring_full_{0};
  std::atomic<uint64_t> readbacks_{0};
  LUID adapter_luid_ = {};
  int texture_width_ = 0;
  int texture_height_ = 0;

  // Readers report from their own render threads.
  Mutex read_fences_lock_;
  // Each reader's fence, opened on this device the first time it reports.
  std::map<uint64_t, Microsoft::WRL::ComPtr<ID3D11Fence>> readers_;

  // Readback is rare, from whichever thread a consumer calls ToI420() on.
  Mutex readback_lock_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
};

}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_CODECS_H264_WIN_D3D11_TEXTURE_RING_H_
