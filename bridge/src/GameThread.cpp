#include "GameThread.hpp"

#include <windows.h>

#include <UE4SSRuntime.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UEngine.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

namespace ue5mcp::bridge
{
    namespace UE = RC::Unreal;

    GameThread& GameThread::instance()
    {
        static GameThread g;
        return g;
    }

    // ------------------------------------------------------------------ SEH guard
    namespace
    {
        constexpr unsigned long kCppExceptionCode = 0xE06D7363; // MSVC C++ exceptions travel as SEH; let them through

        int seh_filter(unsigned long code, EXCEPTION_POINTERS* info, unsigned long* out_code, void** out_addr)
        {
            if (code == kCppExceptionCode) return EXCEPTION_CONTINUE_SEARCH;
            *out_code = code;
            *out_addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
            return EXCEPTION_EXECUTE_HANDLER;
        }

        // No objects with destructors may live in this frame.
        bool seh_call(const std::function<void()>* fn, unsigned long* code, void** address)
        {
            __try
            {
                (*fn)();
                return true;
            }
            __except (seh_filter(GetExceptionCode(), GetExceptionInformation(), code, address))
            {
                return false;
            }
        }
    } // namespace

    bool seh_guard(const std::function<void()>& fn, unsigned long* code, void** address)
    {
        *code = 0;
        *address = nullptr;
        return seh_call(&fn, code, address);
    }

    // ------------------------------------------------------------------ install / drain

    static void tick_pre(UE::Hook::TCallbackIterationData<void>&, UE::UEngine*, float, bool)
    {
        GameThread::instance().drain("tick");
    }

    static void process_event_pre(UE::Hook::TCallbackIterationData<void>&, UE::UObject*, UE::UFunction*, void*)
    {
        GameThread::instance().drain("pe");
    }

    void GameThread::install()
    {
        if (m_installed) return;
        if (RC::UE4SSRuntime::IsEngineTickAvailable())
        {
            UE::Hook::RegisterEngineTickPreCallback(&tick_pre, {false, false, STR("UE5MCP"), STR("GameThread")});
            m_installed = true;
        }
        else
        {
            // Fallback: ProcessEvent fires many times per frame; drain() dedupes by frame counter.
            UE::Hook::RegisterProcessEventPreCallback(&process_event_pre, {false, false, STR("UE5MCP"), STR("GameThread")});
            m_installed = true;
        }
    }

    bool GameThread::is_game_thread() const
    {
        return m_tick_seen && std::this_thread::get_id() == m_game_thread_id;
    }

    void GameThread::drain(const char* source)
    {
        auto t = now_ms();
        if (source[0] == 'p')
        {
            // ProcessEvent fallback: treat ~16ms gaps as frames so frame_ms stays meaningful.
            if (t - m_last_tick_ms < 1) return;
        }
        if (!m_tick_seen)
        {
            m_game_thread_id = std::this_thread::get_id();
            m_tick_seen = true;
        }
        if (m_last_tick_ms) m_frame_ms = static_cast<double>(t - m_last_tick_ms);
        m_last_tick_ms = t;
        ++m_frames;

        // Take all queued jobs; run them one by one. Each gets its own guard so one fault does not drop the rest.
        std::deque<Job> jobs;
        {
            std::lock_guard lock(m_mutex);
            jobs.swap(m_queue);
        }
        for (auto& job : jobs)
        {
            if (job.abandoned->load()) continue;
            json result;
            std::optional<RpcError> error;
            unsigned long code = 0;
            void* addr = nullptr;
            bool ok = seh_guard(
                    [&] {
                        try
                        {
                            result = job.fn();
                        }
                        catch (const RpcError& e)
                        {
                            error = e;
                        }
                        catch (const std::exception& e)
                        {
                            error = RpcError(ExecutionException, e.what(), json{{"kind", "cpp"}});
                        }
                    },
                    &code, &addr);
            if (!ok)
            {
                char buf[160];
                std::snprintf(buf, sizeof buf, "structured exception 0x%08lX at %p inside the call; the game may now be unstable", code, addr);
                error = RpcError(ExecutionException, buf, json{{"kind", "seh"}, {"code", code}, {"address", reinterpret_cast<std::uintptr_t>(addr)}, {"unstable", true}});
                log(buf);
            }
            try
            {
                if (error) job.promise->set_exception(std::make_exception_ptr(*error));
                else job.promise->set_value(std::move(result));
            }
            catch (...)
            {
            }
        }
    }

    json GameThread::run(std::function<json()> fn, int timeout_ms)
    {
        if (!m_installed) throw RpcError(GameThreadUnavailable, "game thread queue not installed yet (Unreal not initialised)");
        if (is_game_thread())
        {
            // Already on the game thread (e.g. called from a hook callback or nested tool): run inline, same guard.
            json result;
            std::optional<RpcError> error;
            unsigned long code = 0;
            void* addr = nullptr;
            bool ok = seh_guard(
                    [&] {
                        try
                        {
                            result = fn();
                        }
                        catch (const RpcError& e)
                        {
                            error = e;
                        }
                        catch (const std::exception& e)
                        {
                            error = RpcError(ExecutionException, e.what(), json{{"kind", "cpp"}});
                        }
                    },
                    &code, &addr);
            if (!ok) throw RpcError(ExecutionException, "structured exception inside the call", json{{"kind", "seh"}, {"code", code}, {"address", reinterpret_cast<std::uintptr_t>(addr)}, {"unstable", true}});
            if (error) throw *error;
            return result;
        }

        Job job{std::move(fn), std::make_shared<std::promise<json>>(), std::make_shared<std::atomic_bool>(false)};
        auto future = job.promise->get_future();
        auto abandoned = job.abandoned;
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(job));
        }
        if (timeout_ms <= 0) timeout_ms = kDefaultTimeoutMs;
        if (future.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready)
        {
            abandoned->store(true);
            auto idle = now_ms() - m_last_tick_ms;
            std::string why = !m_tick_seen ? "the game thread has not ticked yet (still loading?)"
                              : idle > timeout_ms ? "the game thread has not ticked for " + std::to_string(idle) + " ms (paused, loading, or hung)"
                                                  : "the job did not finish in time";
            throw RpcError(Timeout, "timed out after " + std::to_string(timeout_ms) + " ms: " + why, json{{"idle_ms", idle}});
        }
        return future.get(); // rethrows RpcError set by drain()
    }
} // namespace ue5mcp::bridge
