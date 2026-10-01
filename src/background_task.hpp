#pragma once
#include <chrono>
#include <future>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <utility>

// A cancelled OpenCV call may still be finishing. Keep its future alive, discard
// its result, and prevent overlapping replacements without blocking the UI.
template<class T> class BackgroundTask {
    std::future<T> future;
    std::stop_source stop;
public:
    bool pending() const { return future.valid(); }
    bool active() const { return pending() && !stop.stop_requested(); }
    void cancel() { stop.request_stop(); }
    void wait() { if(pending()) future.wait(); }
    template<class F> void start(F work) {
        if(pending()) throw std::logic_error("Previous task is still finishing.");
        stop=std::stop_source{};
        future=std::async(std::launch::async,[work=std::move(work),token=stop.get_token()]() mutable { return work(token); });
    }
    std::optional<T> poll() {
        if(!pending() || future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) return {};
        try {
            auto value=future.get();
            if(!stop.stop_requested()) return value;
        } catch(...) { if(!stop.stop_requested()) throw; }
        return {};
    }
};
