/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_CODECS_H264_WIN_UTILS_CRITSEC_H_
#define MODULES_VIDEO_CODING_CODECS_H264_WIN_UTILS_CRITSEC_H_

#include <WinBase.h>

#include "rtc_base/checks.h"

class CritSec {
 public:
  CRITICAL_SECTION m_criticalSection;

 public:
  // Checked rather than ignored: it fails only when the debug info cannot be
  // allocated, which is under the memory exhaustion these calls already die of,
  // and an uninitialised section makes every later Enter/Leave undefined. Better
  // to name the failure here than to corrupt at the first lock.
  CritSec() {
    RTC_CHECK(InitializeCriticalSectionEx(&m_criticalSection, 100, 0));
  }

  ~CritSec() { DeleteCriticalSection(&m_criticalSection); }

  // Copying would leave two owners of one section, and the second destructor
  // would delete it again.
  CritSec(const CritSec&) = delete;
  CritSec& operator=(const CritSec&) = delete;

  _Acquires_lock_(m_criticalSection) void Lock() {
    EnterCriticalSection(&m_criticalSection);
  }

  _Releases_lock_(m_criticalSection) void Unlock() {
    LeaveCriticalSection(&m_criticalSection);
  }
};

//////////////////////////////////////////////////////////////////////////
//  AutoLock
//  Description: Provides automatic locking and unlocking of a
//               of a critical section.
//
//  Note: The AutoLock object must go out of scope before the CritSec.
//////////////////////////////////////////////////////////////////////////

class AutoLock {
 private:
  CritSec* m_pCriticalSection;

 public:
  _Acquires_lock_(m_pCriticalSection) explicit AutoLock(CritSec& crit) {
    m_pCriticalSection = &crit;
    m_pCriticalSection->Lock();
  }

  _Releases_lock_(m_pCriticalSection) ~AutoLock() {
    m_pCriticalSection->Unlock();
  }

  // A copy would unlock the same section twice.
  AutoLock(const AutoLock&) = delete;
  AutoLock& operator=(const AutoLock&) = delete;
};

#endif  // MODULES_VIDEO_CODING_CODECS_H264_WIN_UTILS_CRITSEC_H_
