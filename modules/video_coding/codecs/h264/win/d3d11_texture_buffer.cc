/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/codecs/h264/win/d3d11_texture_buffer.h"

#include <dxgi1_2.h>

#include <algorithm>

#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
#include "libyuv/convert.h"
#include "modules/video_coding/codecs/h264/win/d3d11_texture_ring.h"
#include "rtc_base/logging.h"
#include "rtc_base/string_utils.h"

using Microsoft::WRL::ComPtr;

namespace webrtc {

namespace {

std::atomic<bool> g_enabled{true};
// Ring and reader ids come from one sequence; neither is ever reused.
std::atomic<uint64_t> g_next_id{1};

}  // namespace

D3D11TextureBuffer::Counters& D3D11TextureBuffer::counters() {
  static Counters counters;
  return counters;
}

void D3D11TextureBuffer::SetEnabled(bool enabled) {
  g_enabled.store(enabled, std::memory_order_relaxed);
}

bool D3D11TextureBuffer::enabled() {
  return g_enabled.load(std::memory_order_relaxed);
}

uint64_t D3D11TextureBuffer::NewReaderId() {
  return g_next_id.fetch_add(1, std::memory_order_relaxed);
}

class D3D11TextureRing::Buffer : public D3D11TextureBuffer {
 public:
  Buffer(rtc::scoped_refptr<D3D11TextureRing> ring,
         int slot,
         uint64_t fence_value,
         int width,
         int height,
         bool bt709,
         bool full_range)
      : ring_(std::move(ring)),
        slot_(slot),
        fence_value_(fence_value),
        width_(width),
        height_(height),
        bt709_(bt709),
        full_range_(full_range) {}

  ~Buffer() override { ring_->ReleaseSlot(slot_); }

  int width() const override { return width_; }
  int height() const override { return height_; }

  rtc::scoped_refptr<I420BufferInterface> ToI420() override {
    counters().readbacks.fetch_add(1, std::memory_order_relaxed);
    if (ring_->readbacks_.fetch_add(1, std::memory_order_relaxed) == 0) {
      RTC_LOG(LS_WARNING) << "Texture ring " << ring_->ring_id_
                          << ": a frame was read back to I420, so something "
                             "is reading call video on the CPU";
    }
    return ring_->ReadBack(slot_, width_, height_);
  }

  uint64_t ring_id() const override { return ring_->ring_id_; }
  int slot() const override { return slot_; }
  HANDLE texture_handle() const override {
    return ring_->slots_[slot_].handle;
  }
  HANDLE fence_handle() const override { return ring_->fence_handle_; }
  uint64_t fence_value() const override { return fence_value_; }
  int texture_width() const override { return ring_->texture_width_; }
  int texture_height() const override { return ring_->texture_height_; }
  LUID adapter_luid() const override { return ring_->adapter_luid_; }
  bool bt709() const override { return bt709_; }
  bool full_range() const override { return full_range_; }

  void AddReadFence(uint64_t reader_id,
                    HANDLE fence,
                    uint64_t value) const override {
    ring_->AddReadFence(slot_, reader_id, fence, value);
  }

 private:
  const rtc::scoped_refptr<D3D11TextureRing> ring_;
  const int slot_;
  const uint64_t fence_value_;
  const int width_;
  const int height_;
  const bool bt709_;
  const bool full_range_;
};

rtc::scoped_refptr<D3D11TextureRing> D3D11TextureRing::Create(
    ID3D11Device* device,
    int texture_width,
    int texture_height) {
  auto ring = rtc::make_ref_counted<D3D11TextureRing>();
  if (!ring->Initialize(device, texture_width, texture_height)) {
    return nullptr;
  }
  return ring;
}

bool D3D11TextureRing::Initialize(ID3D11Device* device,
                                  int texture_width,
                                  int texture_height) {
  device_ = device;
  texture_width_ = texture_width;
  texture_height_ = texture_height;
  ring_id_ = g_next_id.fetch_add(1, std::memory_order_relaxed);

  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5_))) ||
      FAILED(context.As(&context_))) {
    RTC_LOG(LS_WARNING) << "No D3D11.4 fences; decoded frames are read back";
    return false;
  }

  HRESULT hr =
      device5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_));
  if (SUCCEEDED(hr)) {
    hr = fence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                    &fence_handle_);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "No shared fence: 0x" << rtc::ToHex(hr);
    return false;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC adapter_desc = {};
  if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
      SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
      SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
    adapter_luid_ = adapter_desc.AdapterLuid;
  }

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = texture_width;
  desc.Height = texture_height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags =
      D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

  for (Slot& slot : slots_) {
    hr = device->CreateTexture2D(&desc, nullptr, &slot.texture);

    ComPtr<IDXGIResource1> resource;
    if (SUCCEEDED(hr)) {
      hr = slot.texture.As(&resource);
    }
    if (SUCCEEDED(hr)) {
      hr = resource->CreateSharedHandle(
          nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
          nullptr, &slot.handle);
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "No shareable NV12 texture: 0x"
                          << rtc::ToHex(hr);
      return false;
    }
  }

  return true;
}

D3D11TextureRing::~D3D11TextureRing() {
  if (ring_id_ != 0) {
    RTC_LOG(LS_INFO) << "Texture ring " << ring_id_ << " done: "
                     << textures_.load() << " textures, " << ring_full_.load()
                     << " frames read back with the ring full, "
                     << readbacks_.load() << " textures read back";
  }
  for (Slot& slot : slots_) {
    if (slot.handle != nullptr) {
      CloseHandle(slot.handle);
    }
  }
  if (fence_handle_ != nullptr) {
    CloseHandle(fence_handle_);
  }
}

rtc::scoped_refptr<VideoFrameBuffer> D3D11TextureRing::Wrap(
    ID3D11Texture2D* source,
    UINT subresource,
    int width,
    int height,
    bool bt709,
    bool full_range) {
  int slot = -1;
  for (int i = 0; i < kSlots; ++i) {
    const int candidate = (next_slot_ + i) % kSlots;
    bool expected = false;
    if (slots_[candidate].busy.compare_exchange_strong(expected, true)) {
      slot = candidate;
      break;
    }
  }

  if (slot < 0) {
    D3D11TextureBuffer::counters().ring_full.fetch_add(
        1, std::memory_order_relaxed);
    if (ring_full_.fetch_add(1, std::memory_order_relaxed) == 0) {
      RTC_LOG(LS_WARNING) << "Texture ring " << ring_id_
                          << " full; frames are read back until a slot frees";
    }
    return nullptr;
  }

  next_slot_ = (slot + 1) % kSlots;

  {
    MutexLock lock(&read_fences_lock_);
    for (const ReadFence& read : slots_[slot].read_fences) {
      context_->Wait(read.fence, read.value);
    }
    slots_[slot].read_fences.clear();
  }

  // The decoder's texture can be padded beyond the frame, and the slot is the
  // frame rounded up to even, so only that much of it is copied.
  const D3D11_BOX box = {0, 0, 0, static_cast<UINT>(texture_width_),
                         static_cast<UINT>(texture_height_), 1};
  context_->CopySubresourceRegion(slots_[slot].texture.Get(), 0, 0, 0, 0,
                                  source, subresource, &box);

  // Flushed, or a renderer waiting on the fence would wait for whatever else
  // made the decoder's device submit.
  context_->Signal(fence_.Get(), ++fence_value_);
  context_->Flush();

  D3D11TextureBuffer::counters().textures.fetch_add(1,
                                                    std::memory_order_relaxed);
  if (textures_.fetch_add(1, std::memory_order_relaxed) == 0) {
    RTC_LOG(LS_INFO) << "Texture ring " << ring_id_ << ": " << texture_width_
                     << "x" << texture_height_
                     << " frames go out as D3D11 textures";
  }

  return rtc::make_ref_counted<Buffer>(
      rtc::scoped_refptr<D3D11TextureRing>(this), slot, fence_value_, width,
      height, bt709, full_range);
}

void D3D11TextureRing::ReleaseSlot(int slot) {
  slots_[slot].busy.store(false);
}

void D3D11TextureRing::AddReadFence(int slot,
                                    uint64_t reader_id,
                                    HANDLE fence,
                                    uint64_t value) {
  MutexLock lock(&read_fences_lock_);

  ComPtr<ID3D11Fence>& opened = readers_[reader_id];
  if (opened == nullptr) {
    HRESULT hr = device5_->OpenSharedFence(fence, IID_PPV_ARGS(&opened));
    if (FAILED(hr)) {
      // The slot is then reused without waiting for this reader, as it was
      // before readers reported fences at all.
      RTC_LOG(LS_WARNING) << "Couldn't open a reader's fence: 0x"
                          << rtc::ToHex(hr);
      readers_.erase(reader_id);
      return;
    }
  }

  // A reader drawing the same frame twice only needs its later fence waited.
  for (ReadFence& read : slots_[slot].read_fences) {
    if (read.fence == opened.Get()) {
      read.value = std::max(read.value, value);
      return;
    }
  }
  slots_[slot].read_fences.push_back({opened.Get(), value});
}

rtc::scoped_refptr<I420BufferInterface> D3D11TextureRing::ReadBack(int slot,
                                                                   int width,
                                                                   int height) {
  MutexLock lock(&readback_lock_);

  if (staging_ == nullptr) {
    D3D11_TEXTURE2D_DESC desc = {};
    slots_[slot].texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "No staging texture for a readback: 0x"
                        << rtc::ToHex(hr);
      return nullptr;
    }
  }

  context_->CopyResource(staging_.Get(), slots_[slot].texture.Get());

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  HRESULT hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Couldn't map a readback: 0x" << rtc::ToHex(hr);
    return nullptr;
  }

  rtc::scoped_refptr<I420Buffer> buffer = I420Buffer::Create(width, height);

  // NV12: the UV plane follows the full height of the Y plane at the same
  // pitch, which is how a mapped NV12 texture is laid out.
  const uint8_t* src_y = static_cast<const uint8_t*>(mapped.pData);
  const uint8_t* src_uv =
      src_y + static_cast<size_t>(mapped.RowPitch) * texture_height_;

  libyuv::NV12ToI420(src_y, static_cast<int>(mapped.RowPitch), src_uv,
                     static_cast<int>(mapped.RowPitch), buffer->MutableDataY(),
                     buffer->StrideY(), buffer->MutableDataU(),
                     buffer->StrideU(), buffer->MutableDataV(),
                     buffer->StrideV(), width, height);

  context_->Unmap(staging_.Get(), 0);
  return buffer;
}

}  // namespace webrtc
