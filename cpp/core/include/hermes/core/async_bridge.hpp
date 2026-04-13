// async_bridge — minimal async helper for hermes C++ port.
//
// Python's `_run_async` pattern bridges sync callers to async I/O.  In
// C++ we bridge sync callers to a thread pool via std::async.  This
// header deliberately stays header-only so it can be included wherever
// needed without pulling in extra translation units.
//
// Usage:
//   auto fut  = hermes::core::run_async([&]{ return expensive(); });
//   auto vals = hermes::core::join_all(std::move(futures));
//
// For parallel tool dispatch in AIAgent we typically spawn one future
// per tool_call and collect results preserving order.
#pragma once

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace hermes::core {

// Spawn `fn` on a detached worker thread (std::async, launch::async).
// The returned future owns the result; the caller is responsible for
// either `.get()`-ing it or letting the destructor block.
template <typename F, typename... Args>
auto run_async(F&& fn, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>> {
    return std::async(std::launch::async,
                      std::forward<F>(fn),
                      std::forward<Args>(args)...);
}

// Block until every future in `futs` resolves, collecting the results
// into a vector (order preserved).  If any future throws, the first
// exception is rethrown after all peers have finished.  This mirrors
// Python's `asyncio.gather(..., return_exceptions=False)` semantics.
template <typename T>
std::vector<T> join_all(std::vector<std::future<T>> futs) {
    std::vector<T> out;
    out.reserve(futs.size());
    std::exception_ptr first_exc;
    for (auto& f : futs) {
        try {
            out.emplace_back(f.get());
        } catch (...) {
            if (!first_exc) first_exc = std::current_exception();
            // Still push a default-constructed placeholder so indices
            // align with the input; callers that care should use
            // join_all_settled below.
            if constexpr (std::is_default_constructible_v<T>) {
                out.emplace_back();
            }
        }
    }
    if (first_exc) std::rethrow_exception(first_exc);
    return out;
}

// void specialisation — just waits.
inline void join_all(std::vector<std::future<void>> futs) {
    std::exception_ptr first_exc;
    for (auto& f : futs) {
        try {
            f.get();
        } catch (...) {
            if (!first_exc) first_exc = std::current_exception();
        }
    }
    if (first_exc) std::rethrow_exception(first_exc);
}

// "settled" variant — never throws, returns per-future outcomes.  Useful
// when you want to log failures but keep going (e.g. concurrent tool
// calls where a partial result is still actionable).
template <typename T>
struct Settled {
    bool ok = false;
    T value{};
    std::string error;
};

template <typename T>
std::vector<Settled<T>> join_all_settled(std::vector<std::future<T>> futs) {
    std::vector<Settled<T>> out;
    out.reserve(futs.size());
    for (auto& f : futs) {
        Settled<T> s;
        try {
            s.value = f.get();
            s.ok = true;
        } catch (const std::exception& e) {
            s.error = e.what();
        } catch (...) {
            s.error = "unknown";
        }
        out.emplace_back(std::move(s));
    }
    return out;
}

// Block up to `deadline` for every future; futures that didn't finish
// are left untouched (caller can still .get() later).  Returns the
// number of futures that completed.
template <typename T>
std::size_t wait_for_all(const std::vector<std::future<T>>& futs,
                         std::chrono::milliseconds deadline) {
    std::size_t done = 0;
    const auto end = std::chrono::steady_clock::now() + deadline;
    for (const auto& f : futs) {
        auto now = std::chrono::steady_clock::now();
        if (now >= end) break;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(end - now);
        if (f.wait_for(remaining) == std::future_status::ready) {
            ++done;
        }
    }
    return done;
}

}  // namespace hermes::core
