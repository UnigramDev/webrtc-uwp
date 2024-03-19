/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "api/scoped_refptr.h"

#if defined(WEBRTC_VIDEO_CAPTURE_WINRT)
#include "modules/video_capture/windows/device_info_winrt.h"
#include "modules/video_capture/windows/video_capture_winrt.h"
#else
#include "modules/video_capture/windows/video_capture_ds.h"
#endif

#include "rtc_base/ref_counted_object.h"

namespace webrtc {
namespace videocapturemodule {

// static
VideoCaptureModule::DeviceInfo* VideoCaptureImpl::CreateDeviceInfo() {
#if defined(WEBRTC_VIDEO_CAPTURE_WINRT)
  return DeviceInfoWinRT::Create();
#else
  return DeviceInfoDS::Create();
#endif  // WEBRTC_VIDEO_CAPTURE_WINRT
}

rtc::scoped_refptr<VideoCaptureModule> VideoCaptureImpl::Create(
    const char* device_id) {
  if (device_id == nullptr)
    return nullptr;

#if defined(WEBRTC_VIDEO_CAPTURE_WINRT)
  auto capture = rtc::make_ref_counted<VideoCaptureWinRT>();
#else
  // TODO(tommi): Use Media Foundation implementation for Vista and up.
  auto capture = rtc::make_ref_counted<VideoCaptureDS>();
#endif  // WEBRTC_VIDEO_CAPTURE_WINRT

  if (capture->Init(device_id) != 0) {
    return nullptr;
  }

  return capture;
}

}  // namespace videocapturemodule
}  // namespace webrtc
