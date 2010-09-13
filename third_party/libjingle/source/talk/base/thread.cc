/*
 * libjingle
 * Copyright 2004--2005, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef POSIX
#include <time.h>
#endif

#include "talk/base/common.h"
#include "talk/base/logging.h"
#include "talk/base/thread.h"
#include "talk/base/time.h"

#if defined(OSX_USE_COCOA)
#ifndef OSX
#error OSX_USE_COCOA is defined but not OSX
#endif
#include "talk/base/maccocoathreadhelper.h"
#include "talk/base/scoped_autorelease_pool.h"
#endif

#define MSDEV_SET_THREAD_NAME  0x406D1388

namespace talk_base {

ThreadManager g_thmgr;

#ifdef POSIX
pthread_key_t ThreadManager::key_;

ThreadManager::ThreadManager() {
  pthread_key_create(&key_, NULL);
  main_thread_ = WrapCurrentThread();
#if defined(OSX_USE_COCOA)
  InitCocoaMultiThreading();
#endif
}

ThreadManager::~ThreadManager() {
#ifdef OSX_USE_COCOA
  // This is called during exit, at which point apparently no NSAutoreleasePools
  // are available; but we might still need them to do cleanup (or we get the
  // "no autoreleasepool in place, just leaking" warning when exiting).
  ScopedAutoreleasePool pool;
#endif
  UnwrapCurrentThread();
  // Unwrap deletes main_thread_ automatically.
  pthread_key_delete(key_);

}

Thread *ThreadManager::CurrentThread() {
  return (Thread *)pthread_getspecific(key_);
}

void ThreadManager::SetCurrent(Thread *thread) {
  pthread_setspecific(key_, thread);
}
#endif

#ifdef WIN32
DWORD ThreadManager::key_;

ThreadManager::ThreadManager() {
  key_ = TlsAlloc();
  main_thread_ = WrapCurrentThread();
}

ThreadManager::~ThreadManager() {
  UnwrapCurrentThread();
  TlsFree(key_);
}

Thread *ThreadManager::CurrentThread() {
  return (Thread *)TlsGetValue(key_);
}

void ThreadManager::SetCurrent(Thread *thread) {
  TlsSetValue(key_, thread);
}
#endif

// static
Thread *ThreadManager::WrapCurrentThread() {
  Thread* result = CurrentThread();
  if (NULL == result) {
    result = new Thread();
#if defined(WIN32)
    // We explicitly ask for no rights other than synchronization.
    // This gives us the best chance of succeeding.
    result->thread_ = OpenThread(SYNCHRONIZE, FALSE, GetCurrentThreadId());
    if (!result->thread_)
      LOG_GLE(LS_ERROR) << "Unable to get handle to thread.";
#elif defined(POSIX)
    result->thread_ = pthread_self();
#endif
    result->owned_ = false;
    result->started_ = true;
    SetCurrent(result);
  }

  return result;
}

// static
void ThreadManager::UnwrapCurrentThread() {
  Thread* t = CurrentThread();
  if (t && !(t->IsOwned())) {
    // Clears the platform-specific thread-specific storage.
    SetCurrent(NULL);
#ifdef WIN32
    if (!CloseHandle(t->thread_)) {
      LOG_GLE(LS_ERROR) << "When unwrapping thread, failed to close handle.";
    }
#endif
    t->started_ = false;
    delete t;
  }
}

void ThreadManager::Add(Thread *thread) {
  CritScope cs(&crit_);
  threads_.push_back(thread);
}

void ThreadManager::Remove(Thread *thread) {
  CritScope cs(&crit_);
  threads_.erase(std::remove(threads_.begin(), threads_.end(), thread), threads_.end());
}

void ThreadManager::StopAllThreads_() {
  // TODO: In order to properly implement, Threads need to be ref-counted.
  CritScope cs(&g_thmgr.crit_);
  for (size_t i=0; i<g_thmgr.threads_.size(); ++i) {
    g_thmgr.threads_[i]->Stop();
  }
}

struct ThreadInit {
  Thread* thread;
  Runnable* runnable;
};

Thread::Thread(SocketServer* ss)
    : MessageQueue(ss),
      priority_(PRIORITY_NORMAL),
      started_(false),
      has_sends_(false),
#if defined(WIN32)
      thread_(NULL),
#endif
      owned_(true) {
  g_thmgr.Add(this);
}

Thread::~Thread() {
  Stop();
  if (active_)
    Clear(NULL);
  g_thmgr.Remove(this);
}

bool Thread::SleepMs(int milliseconds) {
#ifdef WIN32
  ::Sleep(milliseconds);
  return true;
#else
  // POSIX has both a usleep() and a nanosleep(), but the former is deprecated,
  // so we use nanosleep() even though it has greater precision than necessary.
  struct timespec ts;
  ts.tv_sec = milliseconds / 1000;
  ts.tv_nsec = (milliseconds % 1000) * 1000000;
  int ret = nanosleep(&ts, NULL);
  if (ret != 0) {
    LOG_ERR(LS_WARNING) << "nanosleep() returning early";
    return false;
  }
  return true;
#endif
}

bool Thread::Start(Runnable* runnable) {
  assert(owned_);
  if (!owned_) return false;
  assert(!started_);
  if (started_) return false;

  ThreadInit* init = new ThreadInit;
  init->thread = this;
  init->runnable = runnable;
#if defined(WIN32)
  DWORD flags = 0;
  if (priority_ != PRIORITY_NORMAL) {
    flags = CREATE_SUSPENDED;
  }
  thread_ = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)PreRun, init, flags,
                         NULL);
  if (thread_) {
    if (priority_ != PRIORITY_NORMAL) {
      if (priority_ == PRIORITY_HIGH) {
        ::SetThreadPriority(thread_, THREAD_PRIORITY_HIGHEST);
      } else if (priority_ == PRIORITY_ABOVE_NORMAL) {
        ::SetThreadPriority(thread_, THREAD_PRIORITY_ABOVE_NORMAL);
      } else if (priority_ == PRIORITY_IDLE) {
        ::SetThreadPriority(thread_, THREAD_PRIORITY_IDLE);
      }
      ::ResumeThread(thread_);
    }
  } else {
    return false;
  }
#elif defined(POSIX)
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (priority_ != PRIORITY_NORMAL) {
    if (priority_ == PRIORITY_IDLE) {
      // There is no POSIX-standard way to set a below-normal priority for an
      // individual thread (only whole process), so let's not support it.
      LOG(LS_WARNING) << "PRIORITY_IDLE not supported";
    } else {
      // Set real-time round-robin policy.
      if (pthread_attr_setschedpolicy(&attr, SCHED_RR) != 0) {
        LOG(LS_ERROR) << "pthread_attr_setschedpolicy";
      }
      struct sched_param param;
      if (pthread_attr_getschedparam(&attr, &param) != 0) {
        LOG(LS_ERROR) << "pthread_attr_getschedparam";
      } else {
        // The numbers here are arbitrary.
        if (priority_ == PRIORITY_HIGH) {
          param.sched_priority = 6;           // 6 = HIGH
        } else {
          ASSERT(priority_ == PRIORITY_ABOVE_NORMAL);
          param.sched_priority = 4;           // 4 = ABOVE_NORMAL
        }
        if (pthread_attr_setschedparam(&attr, &param) != 0) {
          LOG(LS_ERROR) << "pthread_attr_setschedparam";
        }
      }
    }
  }
  int error_code = pthread_create(&thread_, &attr, PreRun, init);
  if (0 != error_code) {
    LOG(LS_ERROR) << "Unable to create pthread, error " << error_code;
    return false;
  }
#endif
  started_ = true;
  return true;
}

void Thread::Join() {
  if (started_) {
    ASSERT(!IsCurrent());
#if defined(WIN32)
    WaitForSingleObject(thread_, INFINITE);
    CloseHandle(thread_);
    thread_ = NULL;
#elif defined(POSIX)
    void *pv;
    pthread_join(thread_, &pv);
#endif
    started_ = false;
  }
}

#ifdef WIN32
typedef struct tagTHREADNAME_INFO
{
  DWORD dwType;
  LPCSTR szName;
  DWORD dwThreadID;
  DWORD dwFlags;
} THREADNAME_INFO;

void SetThreadName( DWORD dwThreadID, LPCSTR szThreadName)
{
  THREADNAME_INFO info;
  {
    info.dwType = 0x1000;
    info.szName = szThreadName;
    info.dwThreadID = dwThreadID;
    info.dwFlags = 0;
  }
  __try
  {
    RaiseException(MSDEV_SET_THREAD_NAME, 0, sizeof(info)/sizeof(DWORD), (DWORD*)&info );
  }
  __except(EXCEPTION_CONTINUE_EXECUTION)
  {
  }
}
#endif // WIN32

void* Thread::PreRun(void* pv) {
  ThreadInit* init = static_cast<ThreadInit*>(pv);
  ThreadManager::SetCurrent(init->thread);
#if defined(WIN32) && defined(_DEBUG)
  char buf[256];
  _snprintf(buf, sizeof(buf), "Thread 0x%.8x", init->thread);
  SetThreadName(GetCurrentThreadId(), buf);
#endif
#ifdef OSX_USE_COCOA
  // Make sure the new thread has an autoreleasepool
  ScopedAutoreleasePool pool;
#endif
  if (init->runnable) {
    init->runnable->Run(init->thread);
  } else {
    init->thread->Run();
  }
  delete init;
  return NULL;
}

void Thread::Run() {
  ProcessMessages(kForever);
}

bool Thread::IsOwned() {
  return owned_;
}

void Thread::Stop() {
  MessageQueue::Quit();
  Join();
}

void Thread::Send(MessageHandler *phandler, uint32 id, MessageData *pdata) {
  if (fStop_)
    return;

  // Sent messages are sent to the MessageHandler directly, in the context
  // of "thread", like Win32 SendMessage. If in the right context,
  // call the handler directly.

  Message msg;
  msg.phandler = phandler;
  msg.message_id = id;
  msg.pdata = pdata;
  if (IsCurrent()) {
    phandler->OnMessage(&msg);
    return;
  }

  AutoThread thread;
  Thread *current_thread = Thread::Current();
  ASSERT(current_thread != NULL);  // AutoThread ensures this

  bool ready = false;
  {
    CritScope cs(&crit_);
    EnsureActive();
    _SendMessage smsg;
    smsg.thread = current_thread;
    smsg.msg = msg;
    smsg.ready = &ready;
    sendlist_.push_back(smsg);
    has_sends_ = true;
  }

  // Wait for a reply

  ss_->WakeUp();

  bool waited = false;
  while (!ready) {
    current_thread->ReceiveSends();
    current_thread->socketserver()->Wait(kForever, false);
    waited = true;
  }

  // Our Wait loop above may have consumed some WakeUp events for this
  // MessageQueue, that weren't relevant to this Send.  Losing these WakeUps can
  // cause problems for some SocketServers.
  //
  // Concrete example:
  // Win32SocketServer on thread A calls Send on thread B.  While processing the
  // message, thread B Posts a message to A.  We consume the wakeup for that
  // Post while waiting for the Send to complete, which means that when we exit
  // this loop, we need to issue another WakeUp, or else the Posted message
  // won't be processed in a timely manner.

  if (waited) {
    current_thread->socketserver()->WakeUp();
  }
}

void Thread::ReceiveSends() {
  // Before entering critical section, check boolean.

  if (!has_sends_)
    return;

  // Receive a sent message. Cleanup scenarios:
  // - thread sending exits: We don't allow this, since thread can exit
  //   only via Join, so Send must complete.
  // - thread receiving exits: Wakeup/set ready in Thread::Clear()
  // - object target cleared: Wakeup/set ready in Thread::Clear()
  crit_.Enter();
  while (!sendlist_.empty()) {
    _SendMessage smsg = sendlist_.front();
    sendlist_.pop_front();
    crit_.Leave();
    smsg.msg.phandler->OnMessage(&smsg.msg);
    crit_.Enter();
    *smsg.ready = true;
    smsg.thread->socketserver()->WakeUp();
  }
  has_sends_ = false;
  crit_.Leave();
}

void Thread::Clear(MessageHandler *phandler, uint32 id,
                   MessageList* removed) {
  CritScope cs(&crit_);

  // Remove messages on sendlist_ with phandler
  // Object target cleared: remove from send list, wakeup/set ready
  // if sender not NULL.

  std::list<_SendMessage>::iterator iter = sendlist_.begin();
  while (iter != sendlist_.end()) {
    _SendMessage smsg = *iter;
    if (smsg.msg.Match(phandler, id)) {
      if (removed) {
        removed->push_back(smsg.msg);
      } else {
        delete smsg.msg.pdata;
      }
      iter = sendlist_.erase(iter);
      *smsg.ready = true;
      smsg.thread->socketserver()->WakeUp();
      continue;
    }
    ++iter;
  }

  MessageQueue::Clear(phandler, id, removed);
}

bool Thread::ProcessMessages(int cmsLoop) {
  uint32 msEnd = (kForever == cmsLoop) ? 0 : TimeAfter(cmsLoop);
  int cmsNext = cmsLoop;

  while (true) {
    Message msg;
    if (!Get(&msg, cmsNext))
      return !IsQuitting();
    Dispatch(&msg);

    if (cmsLoop != kForever) {
      cmsNext = TimeUntil(msEnd);
      if (cmsNext < 0)
        return true;
    }
  }
}

AutoThread::AutoThread(SocketServer* ss) : Thread(ss) {
  if (!ThreadManager::CurrentThread()) {
    ThreadManager::SetCurrent(this);
  }
}

AutoThread::~AutoThread() {
  if (ThreadManager::CurrentThread() == this) {
    ThreadManager::SetCurrent(NULL);
  }
}

} // namespace talk_base
