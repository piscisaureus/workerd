// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "heap-arena.h"

#include <workerd/util/memory-class.h>

#include <libplatform/libplatform.h>
#include <v8-platform.h>

#include <kj/common.h>

namespace workerd::jsg {

class WD_MEMORY_CLASS_HOST V8PlatformWrapper: public v8::Platform {
 public:
  explicit V8PlatformWrapper(v8::Platform& inner): inner(inner) {}

  v8::PageAllocator* GetPageAllocator() override {
    return inner.GetPageAllocator();
  }

  int NumberOfWorkerThreads() override {
    return inner.NumberOfWorkerThreads();
  }

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* isolate, v8::TaskPriority priority) override {
    return inner.GetForegroundTaskRunner(isolate, priority);
  }

  // The worker-thread task queues are the platform's, shared by every isolate, and an isolate
  // posts to them from inside its turns (a concurrent compile job, say): the queue entries go
  // in the shared heap, not in the posting isolate's heap arena, where the next isolate's post
  // would fault on them.
  void PostTaskOnWorkerThreadImpl(v8::TaskPriority priority,
      std::unique_ptr<v8::Task> task,
      const v8::SourceLocation& location) override {
    SharedHeapScope sharedHeap;
    inner.PostTaskOnWorkerThreadImpl(priority, kj::mv(task), location);
  }

  void PostDelayedTaskOnWorkerThreadImpl(v8::TaskPriority priority,
      std::unique_ptr<v8::Task> task,
      double delay_in_seconds,
      const v8::SourceLocation& location) override {
    SharedHeapScope sharedHeap;
    inner.PostDelayedTaskOnWorkerThreadImpl(priority, kj::mv(task), delay_in_seconds, location);
  }

  // The job handle is V8's default one, but created over this platform rather than the inner
  // one, so that the tasks the job posts as its concurrency grows go through
  // PostTaskOnWorkerThreadImpl() above. The job's own state is shared with the worker threads
  // that run it, and is allocated in the shared heap for the same reason as the queue entries.
  std::unique_ptr<v8::JobHandle> CreateJobImpl(v8::TaskPriority priority,
      std::unique_ptr<v8::JobTask> job_task,
      const v8::SourceLocation& location) override {
    SharedHeapScope sharedHeap;
    return v8::platform::NewDefaultJobHandle(this, priority,
        std::make_unique<JobTaskWrapper>(kj::mv(job_task)), inner.NumberOfWorkerThreads());
  }

  bool IdleTasksEnabled(v8::Isolate* isolate) override {
    return inner.IdleTasksEnabled(isolate);
  }

  double MonotonicallyIncreasingTime() override {
    return inner.MonotonicallyIncreasingTime();
  }

  int64_t CurrentClockTimeMilliseconds() override {
    return inner.CurrentClockTimeMilliseconds();
  }

  double CurrentClockTimeMillis() override {
    return inner.CurrentClockTimeMillis();
  }

  double CurrentClockTimeMillisecondsHighResolution() override {
    return inner.CurrentClockTimeMillisecondsHighResolution();
  }

  v8::TracingController* GetTracingController() override {
    return inner.GetTracingController();
  }

  v8::ThreadIsolatedAllocator* GetThreadIsolatedAllocator() override {
    // Forward to the inner platform so that V8's ThreadIsolation can use PKU
    // (Memory Protection Keys) to enforce W^X on JIT code pages and
    // write-protect the code pointer tables.  Without this, the
    // DefaultPlatform's allocator (which calls pkey_alloc) was silently
    // dropped and ThreadIsolation was disabled.
    return inner.GetThreadIsolatedAllocator();
  }

 private:
  v8::Platform& inner;

  class JobTaskWrapper: public v8::JobTask {
   public:
    JobTaskWrapper(std::unique_ptr<v8::JobTask> inner);

    void Run(v8::JobDelegate*) override;

    size_t GetMaxConcurrency(size_t worker_count) const override {
      return inner->GetMaxConcurrency(worker_count);
    }

   private:
    std::unique_ptr<v8::JobTask> inner;
  };
};

}  // namespace workerd::jsg
