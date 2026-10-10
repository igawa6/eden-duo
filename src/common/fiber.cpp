// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <mutex>

#include "common/assert.h"
#include "common/fiber.h"

#include <boost/context/detail/fcontext.hpp>

// Portable "are we building under ThreadSanitizer" detection: GCC defines
// __SANITIZE_THREAD__ when -fsanitize=thread is active; Clang exposes the same fact via
// __has_feature(thread_sanitizer). Neither macro exists otherwise, so this must not be
// written as a bare #if on an undefined name (breaks under -Wundef/-Werror) — always define
// EDEN_HAS_TSAN to 0 or 1 explicitly.
#if defined(__SANITIZE_THREAD__)
#define EDEN_HAS_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define EDEN_HAS_TSAN 1
#endif
#endif
#ifndef EDEN_HAS_TSAN
#define EDEN_HAS_TSAN 0
#endif

#if EDEN_HAS_TSAN
#include <sanitizer/tsan_interface.h>
#endif

namespace Common {

#ifdef __OPENORBIS__
constexpr size_t DEFAULT_STACK_SIZE = 128 * 4096;
#else
constexpr size_t DEFAULT_STACK_SIZE = 512 * 4096;
#endif
constexpr u32 CANARY_VALUE = 0xDEADBEEF;

// ThreadSanitizer fiber annotations.
//
// The guest-CPU scheduler in this codebase (cpu_manager / k_thread / k_scheduler) switches
// user-space stacks via boost::context fcontexts, funneled through the single
// Fiber::YieldTo choke point below. TSan has no idea these stack switches are happening and,
// left unannotated, corrupts its own internal thread-slot bookkeeping (observed as a
// TSan-internal "thr->slot != 0" CHECK failure a few seconds into guest boot, well before any
// guest code is at fault). TSan's fiber API (see sanitizer/tsan_interface.h) exists precisely
// to describe this pattern to the race detector: each logical fiber gets an opaque handle via
// __tsan_create_fiber, every stack switch is announced via __tsan_switch_to_fiber immediately
// before it actually happens, and the handle is retired via __tsan_destroy_fiber once the
// fiber is gone. Note the real API is a single __tsan_switch_to_fiber(fiber, flags) call, not
// a start/end pair — no "_start"/"_end" pair exists in either the GCC 16 or Clang 22
// tsan_interface.h headers.
//
// Every call site below (constructor/ThreadToFiber/YieldTo/destructor) is itself wrapped in
// #if EDEN_HAS_TSAN, including the FiberImpl::tsan_fiber member that stores the handle these
// return — not just these helper bodies — so that a non-TSan build has no trace of any of this
// left to optimize away: no extra member, no extra store, no extra call instruction anywhere.
#if EDEN_HAS_TSAN
namespace {
void* TsanCreateFiber() {
    return __tsan_create_fiber(/*flags=*/0);
}
void TsanDestroyFiber(void* tsan_fiber) {
    if (tsan_fiber != nullptr) {
        __tsan_destroy_fiber(tsan_fiber);
    }
}
void TsanSwitchToFiber(void* tsan_fiber) {
    // flags=0: deliberately keep the happens-before edge TSan would otherwise insert here.
    // A YieldTo transfer is a real handoff of shared kernel/scheduler state (KThread fields,
    // KScheduler::m_switch_cur_thread/m_state, PhysicalCore register save/restore) between
    // logical execution contexts; __tsan_switch_to_fiber_no_sync would hide exactly the races
    // this annotation exists to catch.
    __tsan_switch_to_fiber(tsan_fiber, /*flags=*/0);
}
void* TsanGetCurrentFiber() {
    return __tsan_get_current_fiber();
}
} // namespace
#endif

struct Fiber::FiberImpl {
    FiberImpl() {}

    u32 canary_1 = CANARY_VALUE;
    std::array<u8, DEFAULT_STACK_SIZE> stack{};
    std::array<u8, DEFAULT_STACK_SIZE> rewind_stack{};
    u32 canary_2 = CANARY_VALUE;

    boost::context::detail::fcontext_t context{};
    boost::context::detail::fcontext_t rewind_context{};

    std::mutex guard;
    std::function<void()> entry_point;
    std::function<void()> rewind_point;
    std::shared_ptr<Fiber> previous_fiber;

    u8* stack_limit = nullptr;
    u8* rewind_stack_limit = nullptr;
    bool is_thread_fiber = false;
    bool released = false;

#if EDEN_HAS_TSAN
    // Opaque TSan fiber handle for this FiberImpl. Populated either by TsanCreateFiber() (a
    // freshly allocated boost-context stack — see Fiber::Fiber) or by TsanGetCurrentFiber() (a
    // thread-fiber wrapping an OS thread's own native stack — see Fiber::ThreadToFiber); those
    // two origins are why the destructor must not treat them the same way (only the former was
    // ever separately allocated and needs __tsan_destroy_fiber). Compiled out entirely (not
    // just left null) in a non-TSan build, along with every read/write of it below.
    void* tsan_fiber = nullptr;
#endif
};

void Fiber::SetRewindPoint(std::function<void()>&& rewind_func) {
    impl->rewind_point = std::move(rewind_func);
}

Fiber::Fiber(std::function<void()>&& entry_point_func) : impl{std::make_unique<FiberImpl>()} {
#if EDEN_HAS_TSAN
    impl->tsan_fiber = TsanCreateFiber();
#endif
    impl->entry_point = std::move(entry_point_func);
    impl->stack_limit = impl->stack.data();
    impl->rewind_stack_limit = impl->rewind_stack.data();
    u8* stack_base = impl->stack_limit + DEFAULT_STACK_SIZE;
    impl->context = boost::context::detail::make_fcontext(stack_base, impl->stack.size(), [](boost::context::detail::transfer_t transfer) -> void {
        auto* fiber = static_cast<Fiber*>(transfer.data);
        ASSERT(fiber && fiber->impl && fiber->impl->previous_fiber && fiber->impl->previous_fiber->impl);
        ASSERT(fiber->impl->canary_1 == CANARY_VALUE);
        ASSERT(fiber->impl->canary_2 == CANARY_VALUE);
        fiber->impl->previous_fiber->impl->context = transfer.fctx;
        fiber->impl->previous_fiber->impl->guard.unlock();
        fiber->impl->previous_fiber.reset();
        fiber->impl->entry_point();
        UNREACHABLE();
    });
}

Fiber::Fiber() : impl{std::make_unique<FiberImpl>()} {}

Fiber::~Fiber() {
    if (!impl->released) {
        // Make sure the Fiber is not being used
        const bool locked = impl->guard.try_lock();
        ASSERT_MSG(locked, "Destroying a fiber that's still running");
        if (locked) {
            impl->guard.unlock();
        }
    }
#if EDEN_HAS_TSAN
    // Only a created (boost-context-stack) fiber owns a separately allocated TSan handle.
    // A thread-fiber's tsan_fiber came from TsanGetCurrentFiber() (see ThreadToFiber) and
    // represents the OS thread's own built-in TSan context, which TSan retires itself when
    // the real thread exits — destroying it here would be a use-after-free-shaped bug in
    // TSan's own bookkeeping.
    if (!impl->is_thread_fiber) {
        TsanDestroyFiber(impl->tsan_fiber);
    }
#endif
}

void Fiber::Exit() {
    ASSERT_MSG(impl->is_thread_fiber, "Exiting non main thread fiber");
    if (impl->is_thread_fiber) {
        impl->guard.unlock();
        impl->released = true;
    }
}

void Fiber::YieldTo(std::weak_ptr<Fiber> weak_from, Fiber& to) {
    to.impl->guard.lock();
    to.impl->previous_fiber = weak_from.lock();

#if EDEN_HAS_TSAN
    // Must be announced immediately before the actual stack switch below (jump_fcontext is
    // this function's only switch point — see the class-level fiber.h comment on the choke
    // point). This is what makes fiber migration across OS threads (the common,
    // use_multi_core=true case) correct: TSan's fiber handle tracks the logical execution
    // context, not the OS thread, so it does not matter which host thread happens to execute
    // this call.
    TsanSwitchToFiber(to.impl->tsan_fiber);
#endif
    auto transfer = boost::context::detail::jump_fcontext(to.impl->context, &to);
    // "from" might no longer be valid if the thread was killed
    if (auto from = weak_from.lock()) {
        if (from->impl->previous_fiber == nullptr) {
            ASSERT(false && "previous_fiber is nullptr!");
        } else {
            from->impl->previous_fiber->impl->context = transfer.fctx;
            from->impl->previous_fiber->impl->guard.unlock();
            from->impl->previous_fiber.reset();
        }
    }
}

std::shared_ptr<Fiber> Fiber::ThreadToFiber() {
    std::shared_ptr<Fiber> fiber = std::shared_ptr<Fiber>{new Fiber()};
    fiber->impl->guard.lock();
    fiber->impl->is_thread_fiber = true;
#if EDEN_HAS_TSAN
    // This wraps the calling OS thread's own native stack, not a freshly allocated one, so it
    // gets TSan's existing handle for that thread rather than a new __tsan_create_fiber'd one
    // (see the matching comment on FiberImpl::tsan_fiber and on the destructor above).
    fiber->impl->tsan_fiber = TsanGetCurrentFiber();
#endif
    return fiber;
}

} // namespace Common
