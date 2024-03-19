/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CAPTURE_VIDEO_CAPTURE_DEFINES_H_
#define MODULES_VIDEO_CAPTURE_VIDEO_CAPTURE_DEFINES_H_

#if defined(WEBRTC_VIDEO_CAPTURE_WINRT)
#include <wrl/client.h>
#endif  // WEBRTC_VIDEO_CAPTURE_WINRT

#include "api/video/video_frame.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"

namespace webrtc {

enum {
  kVideoCaptureUniqueNameLength = 1024
};  // Max unique capture device name lenght
enum { kVideoCaptureDeviceNameLength = 256 };  // Max capture device name lenght
enum { kVideoCaptureProductIdLength = 128 };   // Max product id length

struct VideoCaptureCapability {
  int32_t width;
  int32_t height;
  int32_t maxFPS;
  VideoType videoType;
  bool interlaced;
#if defined(WEBRTC_VIDEO_CAPTURE_WINRT)
  std::wstring profile_id;
  ::Microsoft::WRL::ComPtr<::IUnknown> media_capture_video_profile;
  ::Microsoft::WRL::ComPtr<::IUnknown> record_media_description;
#endif  // WEBRTC_VIDEO_CAPTURE_WINRT

  VideoCaptureCapability()
      : width(0),
        height(0),
        maxFPS(0),
        videoType(VideoType::kUnknown),
        interlaced(false)
#if defined(WEBRTC_VIDEO_CAPTURE_WINRT)
        , profile_id(L"")
        , media_capture_video_profile(nullptr)
        , record_media_description(nullptr)
#endif  // WEBRTC_VIDEO_CAPTURE_WINRT
  {}

  bool operator!=(const VideoCaptureCapability& other) const {
    if (width != other.width)
      return true;
    if (height != other.height)
      return true;
    if (maxFPS != other.maxFPS)
      return true;
    if (videoType != other.videoType)
      return true;
    if (interlaced != other.interlaced)
      return true;
#if defined(WEBRTC_VIDEO_CAPTURE_WINRT)
    if (profile_id != other.profile_id)
      return true;
    if (media_capture_video_profile.Get() !=
        other.media_capture_video_profile.Get())
      return true;
    if (record_media_description.Get() != other.record_media_description.Get())
      return true;
#endif  // WEBRTC_VIDEO_CAPTURE_WINRT

    return false;
  }
  bool operator==(const VideoCaptureCapability& other) const {
    return !operator!=(other);
  }
};

}  // namespace webrtc

#endif  // MODULES_VIDEO_CAPTURE_VIDEO_CAPTURE_DEFINES_H_
