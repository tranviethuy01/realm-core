////////////////////////////////////////////////////////////////////////////
//
// Copyright 2020 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include <realm/object-store/util/scheduler.hpp>

#include <realm/exceptions.hpp>
#include <realm/util/to_string.hpp>

#include <atomic>
#include <dispatch/dispatch.h>
#include <objc/runtime.h>
#include <pthread.h>

namespace {
struct RefCountedInvocationQueue {
    realm::util::InvocationQueue queue;
    std::atomic<size_t> ref_count = {0};
};
} // namespace

namespace realm::util {

class RunLoopScheduler : public util::Scheduler {
public:
    RunLoopScheduler(CFRunLoopRef run_loop = nullptr);
    ~RunLoopScheduler();

    void invoke(util::UniqueFunction<void()>&&) override;

    bool is_on_thread() const noexcept override;
    bool is_same_as(const Scheduler* other) const noexcept override;
    bool can_invoke() const noexcept override;

private:
    CFRunLoopRef m_runloop;
    CFRunLoopSourceRef m_notify_signal = nullptr;
    RefCountedInvocationQueue& m_queue;

    void release(CFRunLoopSourceRef&);
    void set_callback(CFRunLoopSourceRef&, util::UniqueFunction<void()>);
};

RunLoopScheduler::RunLoopScheduler(CFRunLoopRef run_loop)
    : m_runloop(run_loop ?: CFRunLoopGetCurrent())
    , m_queue(*new RefCountedInvocationQueue)
{
    CFRetain(m_runloop);

    CFRunLoopSourceContext ctx{};
    ctx.info = &m_queue;
    ctx.perform = [](void* info) {
        static_cast<RefCountedInvocationQueue*>(info)->queue.invoke_all();
    };
    ctx.retain = [](const void* info) {
        static_cast<RefCountedInvocationQueue*>(const_cast<void*>(info))
            ->ref_count.fetch_add(1, std::memory_order_relaxed);
        return info;
    };
    ctx.release = [](const void* info) {
        auto ptr = static_cast<RefCountedInvocationQueue*>(const_cast<void*>(info));
        if (ptr->ref_count.fetch_add(-1, std::memory_order_acq_rel) == 1) {
            delete ptr;
        }
    };

    m_notify_signal = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &ctx);
    CFRunLoopAddSource(m_runloop, m_notify_signal, kCFRunLoopDefaultMode);
}

RunLoopScheduler::~RunLoopScheduler()
{
    CFRunLoopSourceInvalidate(m_notify_signal);
    CFRelease(m_notify_signal);
    CFRelease(m_runloop);
}

void RunLoopScheduler::invoke(util::UniqueFunction<void()>&& fn)
{
    m_queue.queue.push(std::move(fn));

    CFRunLoopSourceSignal(m_notify_signal);
    // Signalling the source makes it run the next time the runloop gets
    // to it, but doesn't make the runloop start if it's currently idle
    // waiting for events
    CFRunLoopWakeUp(m_runloop);
}

bool RunLoopScheduler::is_on_thread() const noexcept
{
    return CFRunLoopGetCurrent() == m_runloop;
}

bool RunLoopScheduler::is_same_as(const Scheduler* other) const noexcept
{
    auto o = dynamic_cast<const RunLoopScheduler*>(other);
    return (o && (o->m_runloop == m_runloop));
}

bool RunLoopScheduler::can_invoke() const noexcept
{
    // The main thread may not be in a run loop yet if we're called from
    // something like `applicationDidFinishLaunching:`, but it presumably will
    // be in the future
    if (pthread_main_np())
        return true;

    // Current mode indicates why the current callout from the runloop was made,
    // and is null if a runloop callout isn't currently being processed
    if (auto mode = CFRunLoopCopyCurrentMode(CFRunLoopGetCurrent())) {
        CFRelease(mode);
        return true;
    }
    return false;
}

class DispatchQueueScheduler : public util::Scheduler {
public:
    DispatchQueueScheduler(dispatch_queue_t queue);
    ~DispatchQueueScheduler();

    void invoke(util::UniqueFunction<void()>&&) override;

    bool is_on_thread() const noexcept override;
    bool is_same_as(const Scheduler* other) const noexcept override;
    bool can_invoke() const noexcept override
    {
        return true;
    }

private:
    dispatch_queue_t m_queue = nullptr;
    void (^m_callback)() = nullptr;
};

static const void* c_queue_key = &c_queue_key;

DispatchQueueScheduler::DispatchQueueScheduler(dispatch_queue_t queue)
    : m_queue(queue)
{
    if (__builtin_available(iOS 12.0, macOS 10.14, tvOS 12.0, watchOS 5.0, *)) {
        static auto class_dispatch_queue_serial = objc_getClass("OS_dispatch_queue_serial");
        static auto class_dispatch_queue_main = objc_getClass("OS_dispatch_queue_main");
        auto cls = object_getClass(reinterpret_cast<id>(queue));
        if (cls != class_dispatch_queue_serial && cls != class_dispatch_queue_main) {
            auto msg = util::format(
                "Invalid queue '%1' (%2): Realms can only be confined to serial queues or the main queue.",
                dispatch_queue_get_label(queue) ?: "<nil>", class_getName(cls));
            throw InvalidArgument(msg);
        }
    }
    dispatch_retain(m_queue);
    if (dispatch_queue_get_specific(m_queue, c_queue_key) == nullptr) {
        dispatch_queue_set_specific(m_queue, c_queue_key, queue, nullptr);
    }
}

DispatchQueueScheduler::~DispatchQueueScheduler()
{
    dispatch_release(m_queue);
    if (m_callback)
        Block_release(m_callback);
}

void DispatchQueueScheduler::invoke(util::UniqueFunction<void()>&& fn)
{
    auto ptr = fn.release();
    dispatch_async(m_queue, ^{
        util::UniqueFunction<void()>{ptr}();
    });
}

bool DispatchQueueScheduler::is_on_thread() const noexcept
{
    return dispatch_get_specific(c_queue_key) == m_queue;
}

bool DispatchQueueScheduler::is_same_as(const Scheduler* other) const noexcept
{
    auto o = dynamic_cast<const DispatchQueueScheduler*>(other);
    return (o && (o->m_queue == m_queue));
}

} // namespace realm::util
