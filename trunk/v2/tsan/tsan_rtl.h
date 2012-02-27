//===-- tsan_rtl.h ----------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
// Main internal TSan header file.
//
// Ground rules:
//   - C++ run-time should not be used (static CTORs, RTTI, exceptions, static
//     function-scope locals)
//   - All functions/classes/etc reside in namespace __tsan, except for those
//     declared in tsan_interface.h.
//   - Platform-specific files should be used instead of ifdefs (*).
//   - No system headers included in header files (*).
//   - Platform specific headres included only into platform-specific files (*).
//
//  (*) Except when inlining is critical for performance.
//===----------------------------------------------------------------------===//

#ifndef TSAN_RTL_H
#define TSAN_RTL_H

#include "tsan_clock.h"
#include "tsan_defs.h"
#include "tsan_slab.h"
#include "tsan_sync.h"
#include "tsan_trace.h"

namespace __tsan {

enum StatType {
  StatMop,
  StatMopRead,
  StatMopWrite,
  StatMop1,
  StatMop2,
  StatMop4,
  StatMop8,
  StatShadowProcessed,
  StatShadowZero,
  StatShadowSameSize,
  StatShadowIntersect,
  StatShadowNotIntersect,
  StatShadowSameThread,
  StatShadowAnotherThread,
  StatShadowReplace,
  StatFuncEnter,
  StatFuncExit,
  StatEvents,
  StatCnt,
};

struct ReportDesc;
struct Context;

// This struct is stored in TLS.
struct ThreadState {
  const int tid;
  u64 epoch;
  // Synch epoch represents the threads's epoch before the last synchronization
  // action. It allows to reduce number of shadow state updates.
  // For example, fast_synch_epoch=100, last write to addr X was at epoch=150,
  // if we are processing write to X from the same thread at epoch=200,
  // we do nothing, because both writes happen in the same 'synch epoch'.
  // That is, if another memory access does not race with the former write,
  // it does not race with the latter as well.
  // QUESTION: can we can squeeze this into ThreadState::Fast?
  // E.g. ThreadState::Fast is a 44-bit, 32 are taken by synch_epoch and 12 are
  // taken by epoch between synchs.
  // This way we can save one load from tls.
  u64 fast_synch_epoch;
  Trace trace;
  SlabCache clockslab;
  SlabCache syncslab;
  ThreadClock clock;
  u64 stat[StatCnt];

  explicit ThreadState(Context *ctx, int tid);
};

extern Context *ctx;
extern __thread char cur_thread_placeholder[];

INLINE ThreadState *cur_thread() {
  return reinterpret_cast<ThreadState *>(&cur_thread_placeholder);
}

enum ThreadStatus {
  ThreadStatusInvalid,   // Non-existent thread, data is invalid.
  ThreadStatusCreated,   // Created but not yet running.
  ThreadStatusRunning,   // The thread is currently running.
  ThreadStatusFinished,  // Joinable thread is finished but not yet joined.
  ThreadStatusDead,      // Joined, but some info (trace) is still alive.
};

// An info about a thread that is hold for some time after its termination.
struct ThreadDeadInfo {
  Trace trace;
};

struct ThreadContext {
  const int tid;
  ThreadState *thr;
  ThreadStatus status;
  uptr uid;  // Some opaque user thread id.
  bool detached;
  int reuse_count;
  SyncClock sync;
  // Epoch at which the thread had started.
  // If we see an event from the thread stamped by an older epoch,
  // the event is from a dead thread that shared tid with this thread.
  u64 epoch0;
  ThreadDeadInfo dead_info;
  ThreadContext* dead_next;  // In dead thread list.

  explicit ThreadContext(int tid);
};

struct Context {
  Context();

  SlabAlloc clockslab;
  SlabAlloc syncslab;
  SyncTab synctab;

  Mutex report_mtx;
  int nreported;

  Mutex thread_mtx;
  int thread_seq;
  ThreadContext *threads[kMaxTid];
  int dead_list_size;
  ThreadContext* dead_list_head;
  ThreadContext* dead_list_tail;

  u64 stat[StatCnt];
};

void ALWAYS_INLINE INLINE StatInc(ThreadState *thr, StatType typ, u64 n = 1) {
  if (kCollectStats)
    thr->stat[typ] += n;
}

void InitializeShadowMemory();
void InitializeInterceptors();
void Printf(const char *format, ...);
void Report(const char *format, ...);
void Die() NORETURN;

#ifdef TSAN_DEBUG_OUTPUT
# define DPrintf Printf
#else
# define DPrintf(...)
#endif

void Initialize(ThreadState *thr);
int Finalize(ThreadState *thr);

bool MemoryAccess(ThreadState *thr, uptr pc, uptr addr,
                  int size, bool is_write);
void MemoryAccessRange(ThreadState *thr, uptr pc, uptr addr,
                       uptr size, bool is_write);

void FuncEntry(ThreadState *thr, uptr pc);
void FuncExit(ThreadState *thr);

int ThreadCreate(ThreadState *thr, uptr pc, uptr uid, bool detached);
void ThreadStart(ThreadState *thr, int tid);
void ThreadFinish(ThreadState *thr);
void ThreadJoin(ThreadState *thr, uptr pc, uptr uid);
void ThreadDetach(ThreadState *thr, uptr pc, uptr uid);

void MutexCreate(ThreadState *thr, uptr pc, uptr addr, bool rw, bool recursive);
void MutexDestroy(ThreadState *thr, uptr pc, uptr addr);
void MutexLock(ThreadState *thr, uptr pc, uptr addr);
void MutexUnlock(ThreadState *thr, uptr pc, uptr addr);
void MutexReadLock(ThreadState *thr, uptr pc, uptr addr);
void MutexReadUnlock(ThreadState *thr, uptr pc, uptr addr);
void MutexReadOrWriteUnlock(ThreadState *thr, uptr pc, uptr addr);

void Acquire(ThreadState *thr, uptr pc, uptr addr);
void Release(ThreadState *thr, uptr pc, uptr addr);

void internal_memset(void *ptr, int c, uptr size);
void internal_memcpy(void *dst, const void *src, uptr size);

void TraceSwitch(ThreadState *thr) NOINLINE;
void ALWAYS_INLINE INLINE TraceAddEvent(ThreadState *thr, u64 epoch,
                                        EventType typ, uptr addr) {
  StatInc(thr, StatEvents);
  if (UNLIKELY((epoch % (kTraceSize / kTraceParts)) == 0))
    TraceSwitch(thr);
  Event *evp = &thr->trace.events[epoch % kTraceSize];
  Event ev = (u64)addr | ((u64)typ << 61);
  *evp = ev;
}

}  // namespace __tsan

#endif  // TSAN_RTL_H
