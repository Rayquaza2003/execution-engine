#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#include "core/spsc.hpp"

// Asynchronous logger. The hot path never formats, locks, allocates or
// touches a file: log() copies a timestamp, the format string's address and
// the raw argument bytes into a lock-free ring, and a background writer
// thread formats and writes them later. Each producing thread gets its own
// Channel (a single-producer ring), so threads never contend.
//
// Arguments must be numbers or string literals: only the pointer is copied,
// so a string has to outlive the write. When a ring is full the message is
// dropped and counted rather than blocking the caller.
class Logger {
public:
    class Channel {
    public:
        template <class... A>
        bool log(const char* fmt, A... a) {
            static_assert(sizeof...(A) <= 6, "at most 6 arguments");
            static_assert(((std::is_arithmetic_v<A> || std::is_same_v<A, const char*>) && ...),
                          "numbers or string literals only");
            Rec r;
            r.ts = now_ns();
            r.fmt = fmt;
            r.print = &print<A...>;
            size_t off = 0;
            ((std::memcpy(r.args + off, &a, sizeof a), off += sizeof a), ...);
            if (q_.try_push(r)) return true;
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    private:
        friend class Logger;
        struct Rec {
            uint64_t ts;
            const char* fmt;
            void (*print)(std::FILE*, const Rec&);
            alignas(8) unsigned char args[48];
        };

        template <class... A>
        static void print(std::FILE* f, const Rec& r) {
            std::fprintf(f, "%llu ", (unsigned long long)r.ts);
            if constexpr (sizeof...(A) == 0) {
                std::fputs(r.fmt, f);
            } else {
                size_t off = 0;
                auto next = [&]<class T>() {
                    T v;
                    std::memcpy(&v, r.args + off, sizeof v);
                    off += sizeof v;
                    return v;
                };
                std::tuple<A...> args{next.template operator()<A>()...};  // braces: left to right
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
                std::apply([&](auto... x) { std::fprintf(f, r.fmt, x...); }, args);
#pragma GCC diagnostic pop
            }
            std::fputc('\n', f);
        }

        Spsc<Rec, 1 << 14> q_;
        std::atomic<uint64_t> dropped_{0};
    };

    // `channels` producing threads, one Channel each; writes go to out.
    Logger(std::FILE* out, size_t channels) : out_(out) {
        for (size_t i = 0; i < channels; ++i) ch_.push_back(std::make_unique<Channel>());
        writer_ = std::thread([this] { run(); });
    }
    ~Logger() {
        stop_.store(true, std::memory_order_release);
        writer_.join();
        std::fflush(out_);
    }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    Channel& channel(size_t i) { return *ch_[i]; }
    uint64_t written() const { return written_.load(std::memory_order_relaxed); }

    static uint64_t now_ns() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
    }

private:
    // Drain every channel; sleep briefly when there was nothing, so an idle
    // logger doesn't burn a core. Stop only once stop is set and all is drained.
    void run() {
        Channel::Rec r;
        for (;;) {
            bool stopping = stop_.load(std::memory_order_acquire);
            size_t n = 0;
            for (auto& c : ch_)
                while (c->q_.try_pop(r)) r.print(out_, r), ++n;
            written_.fetch_add(n, std::memory_order_relaxed);
            if (stopping && !n) return;
            if (!n) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    std::FILE* out_;
    std::vector<std::unique_ptr<Channel>> ch_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> written_{0};
    std::thread writer_;
};
