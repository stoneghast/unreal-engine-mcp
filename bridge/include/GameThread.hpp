// Runs work on the game thread with a timeout. Drained from the engine tick pre-hook; falls back to
// the ProcessEvent pre-hook when a game has no usable EngineTick signature.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>

#include "BridgeCommon.hpp"

namespace ue5mcp::bridge
{
    class GameThread
    {
      public:
        static GameThread& instance();

        // Install the tick hooks. Call from on_unreal_init.
        void install();
        bool available() const { return m_installed.load(); }
        bool is_game_thread() const;

        // Run fn on the game thread and wait up to timeout_ms. Exceptions (RpcError, std::exception,
        // and SEH faults) propagate to the caller as RpcError. If fn is already on the game thread it runs inline.
        json run(std::function<json()> fn, int timeout_ms);

        // Frame statistics for bridge_health.
        double frame_ms() const { return m_frame_ms.load(); }
        std::uint64_t frame_count() const { return m_frames.load(); }

        // Called from the hooks; public so the static callbacks can reach it.
        void drain(const char* source);

      private:
        struct Job
        {
            std::function<json()> fn;
            std::shared_ptr<std::promise<json>> promise;
            std::shared_ptr<std::atomic_bool> abandoned;
        };
        std::mutex m_mutex;
        std::deque<Job> m_queue;
        std::atomic_bool m_installed{false};
        std::atomic_bool m_tick_seen{false};
        std::atomic<double> m_frame_ms{0};
        std::atomic<std::uint64_t> m_frames{0};
        std::int64_t m_last_tick_ms{0};
        std::uint64_t m_last_pe_frame{~0ull};
        std::thread::id m_game_thread_id{};
    };

    // Execute fn under a structured exception handler. Returns true on success. On a fault, fills code/address.
    bool seh_guard(const std::function<void()>& fn, unsigned long* code, void** address);
} // namespace ue5mcp::bridge
