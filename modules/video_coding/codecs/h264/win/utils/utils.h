/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_H264_WIN_UTILS_UTILS_H_
#define MODULES_VIDEO_CODING_CODECS_H264_WIN_UTILS_UTILS_H_

// Wrapped in do/while so the macro is one statement. Without it,
// `if (c) ON_SUCCEEDED(x); else y;` binds the else to the macro's own if.
#ifdef _DEBUG
#define ON_SUCCEEDED(act)                        \
  do {                                           \
    if (SUCCEEDED(hr)) {                         \
      hr = (act);                                \
      if (FAILED(hr)) {                          \
        RTC_LOG(LS_WARNING) << "ERROR:" << #act; \
        __debugbreak();                          \
      }                                          \
    }                                            \
  } while (0)
#else
#define ON_SUCCEEDED(act)                        \
  do {                                           \
    if (SUCCEEDED(hr)) {                         \
      hr = (act);                                \
      if (FAILED(hr)) {                          \
        RTC_LOG(LS_WARNING) << "ERROR:" << #act; \
      }                                          \
    }                                            \
  } while (0)
#endif

#endif  // MODULES_VIDEO_CODING_CODECS_H264_WIN_UTILS_UTILS_H_
