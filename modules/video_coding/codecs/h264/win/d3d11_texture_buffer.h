/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_H264_WIN_D3D11_TEXTURE_BUFFER_H_
#define MODULES_VIDEO_CODING_CODECS_H264_WIN_D3D11_TEXTURE_BUFFER_H_

#include <Windows.h>

#include <atomic>
#include <cstdint>

#include "api/video/video_frame_buffer.h"

namespace webrtc {

// A decoded NV12 frame that stays on the GPU, in a texture the decoder shares
// by NT handle. A renderer on the same adapter opens the handle on its own
// device, waits on the fence, and draws the texture; anything else calls
// ToI420(), which reads the frame back exactly as the decoder used to.
//
// webrtc is built without RTTI, so a sink recognises this buffer by
// type() == kNative followed by Is(). Nothing else in this build produces
// kNative frames on the receive path.
class D3D11TextureBuffer : public VideoFrameBuffer {
 public:
  static constexpr uint32_t kMagic = 0x44334454;  // 'D3DT'

  static const D3D11TextureBuffer* Is(const VideoFrameBuffer* buffer) {
    if (buffer == nullptr || buffer->type() != Type::kNative) {
      return nullptr;
    }
    const auto* texture = static_cast<const D3D11TextureBuffer*>(buffer);
    return texture->magic_ == kMagic ? texture : nullptr;
  }

  // Identifies the texture across handle reuse: an NT handle value can come
  // back for a different texture once a decoder's ring is gone, so a sink
  // caching what it opened keys on this pair, never on the handle.
  virtual uint64_t ring_id() const = 0;
  virtual int slot() const = 0;

  virtual HANDLE texture_handle() const = 0;
  virtual HANDLE fence_handle() const = 0;
  // The texture is complete once the fence reaches this value.
  virtual uint64_t fence_value() const = 0;

  // The texture can be larger than the frame: it is rounded up to even sizes,
  // which NV12 requires. width() and height() are the visible area, from the
  // texture's top left corner.
  virtual int texture_width() const = 0;
  virtual int texture_height() const = 0;

  virtual LUID adapter_luid() const = 0;

  // As the transform reported it. BT.601 studio range when it reported nothing,
  // which is what H.264 from every other Telegram client is.
  virtual bool bt709() const = 0;
  virtual bool full_range() const = 0;

  // A reader that drew the texture on a device of its own reports here that it
  // is finished once `fence` reaches `value`. The decoder's next copy into this
  // slot waits for that on the GPU, as a write into a Chromium shared image
  // waits on its readers' fences. Called before the reader lets go of the
  // buffer; `reader_id`, from NewReaderId(), keys the opened fence.
  virtual void AddReadFence(uint64_t reader_id,
                            HANDLE fence,
                            uint64_t value) const = 0;

  static uint64_t NewReaderId();

  Type type() const override { return Type::kNative; }

  struct Counters {
    // Frames handed out as textures.
    std::atomic<uint64_t> textures{0};
    // Frames read back in the decoder instead, because no ring slot was free.
    std::atomic<uint64_t> ring_full{0};
    // ToI420() on a texture frame: some consumer needed CPU pixels anyway.
    std::atomic<uint64_t> readbacks{0};
  };

  // Process wide, so an app can log them once per call.
  static Counters& counters();

  // On by default. Off makes the decoder read every frame back, as it did before.
  static void SetEnabled(bool enabled);
  static bool enabled();

 protected:
  D3D11TextureBuffer() = default;

 private:
  const uint32_t magic_ = kMagic;
};

}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_CODECS_H264_WIN_D3D11_TEXTURE_BUFFER_H_
