// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

#pragma once

// C++20 coroutines for uniapi: write asynchronous flows top to bottom with co_await.
//
//   uniapi::Task<> deleteFlow(uniapi::App& app) {
//       uniapi::Result name = co_await app.callAsync("askName");
//       uniapi::Result ok = co_await app.callAsync("confirm", {"Delete " + name.value.get<std::string>() + "?"});
//       if (ok.ok && ok.value.get<bool>()) co_await app.callAsync("showMessage", {"Deleted"});
//   }
//
//   // An exposed coroutine answers the page's call when it finishes (co_return), or rejects it
//   // when it throws. Its parameters must be values, not references.
//   app.expose("slowAdd", [&app](int a, int b) -> uniapi::Task<int> {
//       co_await app.sleep(std::chrono::milliseconds(100));
//       co_return a + b;
//   });
//
//   // Heavy work off the UI thread, then back to it before touching the App:
//   co_await uniapi::switchToBackground();
//   auto result = Crunch(data);
//   co_await app.switchToUi();
//
// A Task starts running at once and runs until its first co_await. It can be awaited by one
// other coroutine (co_await task), which then gets its value or exception; or simply be
// dropped, and it still runs to the end. An exception nobody awaits is printed to stderr.
// A coroutine still waiting in app.sleep(), app.callAsync() or app.switchToUi() when the App
// is destroyed is never resumed, so its frame (and whatever it holds) is never freed. That is
// harmless when the program is exiting anyway; otherwise let such flows finish first.
// The library itself stays C++17; only code including this header needs C++20.

#include <uniapi/uniapi.hpp>

#if !defined(__cpp_impl_coroutine)
#error "uniapi/coro.hpp needs C++20 coroutines (e.g. target_compile_features(<target> PRIVATE cxx_std_20))"
#endif

#include <coroutine>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace uniapi {

template <typename T = void>
class Task;

namespace detail {

// What a Task's coroutine and the Task object share: the outcome, and who to tell. A task can
// finish on another thread (after switchToBackground) while the UI thread attaches a waiter,
// so done / continuation / onDone change only under the mutex.
template <typename T>
struct TaskState {
    std::optional<std::conditional_t<std::is_void_v<T>, bool, T>> value;
    std::exception_ptr error;
    std::mutex mutex;
    bool done = false;
    bool observed = false;                     // someone took the outcome
    std::coroutine_handle<> continuation;      // a coroutine waiting in co_await task
    std::function<void(TaskState&)> onDone;    // or a callback (App::expose answering a call)

    void Finish() {
        std::coroutine_handle<> next;
        std::function<void(TaskState&)> callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
            next = continuation;
            callback = std::move(onDone);
            if (next || callback) observed = true;
        }
        if (next) {
            next.resume();  // the waiting coroutine's own promise catches what it throws
        } else if (callback) {
            // Called from final_suspend, which is noexcept: an escaping exception would end the
            // program.
            try {
                callback(*this);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "uniapi: a Task's completion callback failed: %s\n", e.what());
            } catch (...) {
                std::fprintf(stderr, "uniapi: a Task's completion callback failed\n");
            }
        }
    }

    // A task can finish (even fail) before its Task is awaited, so an exception is reported
    // only when the last owner lets go of it without anyone having taken it.
    ~TaskState() {
        if (!error || observed) return;
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "uniapi: exception in a Task nobody awaited: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "uniapi: unknown exception in a Task nobody awaited\n");
        }
    }
};

template <typename T>
struct TaskPromiseBase {
    std::shared_ptr<TaskState<T>> state = std::make_shared<TaskState<T>>();

    std::suspend_never initial_suspend() noexcept { return {}; }  // starts at once
    // Report the outcome, then let the frame be destroyed: the state outlives it.
    auto final_suspend() noexcept {
        struct Finisher {
            std::shared_ptr<TaskState<T>> state;
            bool await_ready() noexcept {
                state->Finish();
                return true;
            }
            void await_suspend(std::coroutine_handle<>) noexcept {}
            void await_resume() noexcept {}
        };
        return Finisher{state};
    }
    void unhandled_exception() noexcept { state->error = std::current_exception(); }
};

template <typename T>
struct TaskPromise : TaskPromiseBase<T> {
    Task<T> get_return_object();
    template <typename V>
    void return_value(V&& value) {
        this->state->value.emplace(std::forward<V>(value));
    }
};

template <>
struct TaskPromise<void> : TaskPromiseBase<void> {
    Task<void> get_return_object();
    void return_void() { this->state->value.emplace(true); }
};

}  // namespace detail

template <typename T>
class Task {
public:
    using promise_type = detail::TaskPromise<T>;

    explicit Task(std::shared_ptr<detail::TaskState<T>> state) : state_(std::move(state)) {}

    bool done() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->done;
    }

    // co_await task: waits for it to finish; gives its value or rethrows its exception.
    bool await_ready() const { return done(); }
    bool await_suspend(std::coroutine_handle<> waiting) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->done) return false;  // finished in the meantime: carry on at once
        state_->continuation = waiting;
        return true;
    }
    T await_resume() {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->observed = true;
        }
        if (state_->error) std::rethrow_exception(state_->error);
        if constexpr (!std::is_void_v<T>) return std::move(*state_->value);
    }

    // Calls fn(state) when the task finishes (now, if it already has), on the thread that
    // finishes it. One callback.
    void whenDone(std::function<void(detail::TaskState<T>&)> fn) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->done) {
                state_->onDone = std::move(fn);
                return;
            }
            state_->observed = true;
        }
        fn(*state_);
    }

private:
    std::shared_ptr<detail::TaskState<T>> state_;
};

namespace detail {

template <typename T>
Task<T> TaskPromise<T>::get_return_object() {
    return Task<T>(this->state);
}
inline Task<void> TaskPromise<void>::get_return_object() { return Task<void>(this->state); }

// App::expose: a function returning Task<T> answers the call when the task finishes.
template <typename T>
struct task_traits<Task<T>> : std::true_type {
    // keepAlive owns the exposed function object: the coroutine's frame refers to it (its
    // captures) until it finishes, even if the name is exposed again meanwhile.
    static void answer(Task<T> task, const Reply& reply, std::shared_ptr<const void> keepAlive) {
        task.whenDone([reply, keepAlive = std::move(keepAlive)](TaskState<T>& state) {
            if (state.error) {
                try {
                    std::rethrow_exception(state.error);
                } catch (const std::exception& e) {
                    reply.reject(e.what());
                } catch (...) {
                    reply.reject("unknown C++ exception");
                }
            } else if constexpr (std::is_void_v<T>) {
                reply.resolve();
            } else {
                json value;
                try {
                    value = json(std::move(*state.value));  // to_json may throw
                } catch (const std::exception& e) {
                    reply.reject(e.what());
                    return;
                }
                reply.resolve(std::move(value));
            }
        });
    }
};

}  // namespace detail

// co_await uniapi::switchToBackground(): continues on a new background thread. Use the App
// again only after co_await app.switchToUi().
inline auto switchToBackground() {
    struct Awaiter {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) {
            std::thread([handle] { handle.resume(); }).detach();
        }
        void await_resume() const noexcept {}
    };
    return Awaiter{};
}

}  // namespace uniapi
