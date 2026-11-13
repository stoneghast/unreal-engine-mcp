// Function hooks with a call ring buffer, and class-wide tracing through the global ProcessEvent/ProcessInternal callbacks.
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <set>

#include "Bridge.hpp"
#include "GameThread.hpp"
#include "Handles.hpp"
#include "Marshal.hpp"
#include "Registry.hpp"

#pragma warning(push)
#pragma warning(disable : 4005)
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <Unreal/UnrealVersion.hpp>
#pragma warning(pop)

using namespace ue5mcp::schema;

namespace ue5mcp::bridge
{
    using namespace RC::Unreal;

    namespace
    {
        // ------------------------------------------------------------ shared ring buffer
        struct Event
        {
            std::uint64_t seq;
            std::int64_t t_ms;
            std::uint64_t hook_id; // 0 for trace events
            std::uint64_t trace_id;
            std::string source;    // "hook", "ProcessEvent", "ProcessInternal", "ProcessLocalScriptFunction"
            std::string phase;     // "pre" / "post" / "call"
            std::string function;
            std::string context;
            std::string context_class;
            int depth;
            json args;
            json out;
            json ret;
            json caller;
        };

        std::mutex g_ring_mutex;
        std::deque<Event> g_ring;
        std::atomic<std::uint64_t> g_seq{0};

        void push_event(Event e)
        {
            e.seq = ++g_seq;
            std::lock_guard lock(g_ring_mutex);
            g_ring.push_back(std::move(e));
            auto cap = BridgeMod::instance() ? BridgeMod::instance()->config().hook_ring_capacity : 20000;
            while (g_ring.size() > cap) g_ring.pop_front();
        }

        json event_json(const Event& e)
        {
            json j{{"seq", e.seq}, {"t_ms", e.t_ms}, {"source", e.source}, {"phase", e.phase}, {"function", e.function}, {"context", e.context}, {"context_class", e.context_class}, {"depth", e.depth}};
            if (e.hook_id) j["hook_id"] = e.hook_id;
            if (e.trace_id) j["trace_id"] = e.trace_id;
            if (!e.args.is_null()) j["args"] = e.args;
            if (!e.out.is_null()) j["out"] = e.out;
            if (!e.ret.is_null()) j["return"] = e.ret;
            if (!e.caller.is_null()) j["caller"] = e.caller;
            return j;
        }

        // ------------------------------------------------------------ UFunction hooks
        struct HookData;
        void pre_hook(UnrealScriptFunctionCallableContext& context, void* custom_data);
        void post_hook(UnrealScriptFunctionCallableContext& context, void* custom_data);
        using HookId = CallbackId;

        struct HookData
        {
            std::uint64_t id;
            UFunction* function;
            std::string function_path;
            HookId pre_id{};
            HookId post_id{};
            bool has_pre{}, has_post{};
            bool record_pre{true}, record_post{true};
            bool script{false};
            bool capture_args{true}, capture_return{true};
            bool once{false};
            std::atomic_bool enabled{true};
            std::atomic<std::uint64_t> calls{0};
            UObject* context_filter{nullptr};
            MarshalOptions opt;
            FProperty* return_property{nullptr};
        };

        std::mutex g_hooks_mutex;
        std::map<std::uint64_t, std::shared_ptr<HookData>> g_hooks;
        // Blueprint (non-native) functions cannot take a per-function native hook: UE4SS patches UFunction::Func, which for
        // script functions is ProcessInternal itself. They are matched inside the global ProcessInternal callbacks instead.
        std::vector<std::shared_ptr<HookData>> g_script_hooks;
        void ensure_trace_hooks();
        std::uint64_t g_next_hook{1};
        thread_local int t_hook_depth = 0;

        json caller_of(FFrame& stack)
        {
            // FFrame::PreviousFrame is the script caller when a Blueprint called us. Null for native callers.
            FFrame* prev = stack.PreviousFrame();
            if (!prev) return nullptr;
            json j = json::object();
            if (auto* node = prev->Node()) j["function"] = Handles::path_of(node);
            if (auto* obj = prev->Object()) j["object"] = Handles::path_of(obj);
            return j;
        }

        json collect_params(UFunction* fn, FFrame& stack, bool outs, const MarshalOptions& opt)
        {
            json out = json::object();
            auto return_offset = fn->GetReturnValueOffset();
            for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated))
            {
                if (!p->HasAnyPropertyFlags(CPF_Parm)) continue;
                if (p->HasAnyPropertyFlags(CPF_ReturnParm) || (return_offset != 0xFFFF && p->GetOffset_Internal() == return_offset)) continue;
                bool is_out = p->HasAnyPropertyFlags(CPF_OutParm) && !p->HasAnyPropertyFlags(CPF_ConstParm);
                if (is_out != outs) continue;
                void* data = is_out ? FindOutParamValueAddress(stack, p) : (stack.Locals() ? p->ContainerPtrToValuePtr<void>(stack.Locals()) : nullptr);
                if (!data) continue;
                out[to_utf8(p->GetName())] = value_to_json(p, data, opt, opt.depth);
            }
            return out;
        }

        void pre_hook(UnrealScriptFunctionCallableContext& context, void* custom_data)
        {
            auto* h = static_cast<HookData*>(custom_data);
            if (!h || !h->enabled || !h->record_pre) return;
            if (h->context_filter && context.Context != h->context_filter) return;
            ++h->calls;
            Event e{};
            e.t_ms = now_ms();
            e.hook_id = h->id;
            e.source = "hook";
            e.phase = "pre";
            e.function = h->function_path;
            e.context = Handles::path_of(context.Context);
            e.context_class = Handles::class_name_of(context.Context);
            e.depth = t_hook_depth++;
            if (h->capture_args) e.args = collect_params(h->function, context.TheStack, false, h->opt);
            e.caller = caller_of(context.TheStack);
            push_event(std::move(e));
        }

        void post_hook(UnrealScriptFunctionCallableContext& context, void* custom_data)
        {
            auto* h = static_cast<HookData*>(custom_data);
            if (!h || !h->enabled) return;
            if (h->context_filter && context.Context != h->context_filter) return;
            if (t_hook_depth > 0) --t_hook_depth;
            if (!h->record_post) { if (h->once) h->enabled = false; return; }
            Event e{};
            e.t_ms = now_ms();
            e.hook_id = h->id;
            e.source = "hook";
            e.phase = "post";
            e.function = h->function_path;
            e.context = Handles::path_of(context.Context);
            e.context_class = Handles::class_name_of(context.Context);
            e.depth = t_hook_depth;
            if (h->capture_args) e.out = collect_params(h->function, context.TheStack, true, h->opt);
            if (h->capture_return && h->return_property && context.RESULT_DECL) e.ret = value_to_json(h->return_property, context.RESULT_DECL, h->opt, h->opt.depth);
            push_event(std::move(e));
            if (h->once) h->enabled = false; // stays registered (removal must not happen inside the callback); hook_remove cleans up
        }

        // ------------------------------------------------------------ class-wide tracing
        struct Trace
        {
            std::uint64_t id;
            std::atomic_bool running{true};
            std::int64_t deadline_ms;
            std::set<UClass*> classes;
            std::string function_glob;
            bool capture_args{false};
            std::size_t max_events{5000};
            std::set<std::string> sources;
            std::atomic<std::uint64_t> count{0};
            std::uint64_t first_seq{0};
            MarshalOptions opt;
        };
        std::mutex g_trace_mutex;
        std::map<std::uint64_t, std::shared_ptr<Trace>> g_traces;
        std::shared_ptr<Trace> g_active_trace; // one at a time keeps the hot path cheap
        std::uint64_t g_next_trace{1};
        std::atomic_bool g_trace_hooks_installed{false};
        thread_local int t_trace_depth = 0;

        void trace_record(const char* source, UObject* context, UFunction* function, void* parms)
        {
            auto trace = g_active_trace; // atomic-ish copy of shared_ptr under no lock: acceptable, trace lifetime is owned by g_traces
            if (!trace || !trace->running || !function) return;
            if (now_ms() > trace->deadline_ms || trace->count >= trace->max_events)
            {
                trace->running = false;
                return;
            }
            if (!trace->sources.contains(source)) return;
            if (!trace->classes.empty())
            {
                bool match = false;
                if (context)
                    for (auto* c : trace->classes)
                        if (context->IsA(c)) { match = true; break; }
                if (!match) return;
            }
            if (!trace->function_glob.empty() && !glob_match(trace->function_glob, to_utf8(function->GetName()))) return;
            ++trace->count;
            Event e{};
            e.t_ms = now_ms();
            e.trace_id = trace->id;
            e.source = source;
            e.phase = "call";
            e.function = Handles::path_of(function);
            e.context = context ? to_utf8(context->GetName()) : "";
            e.context_class = Handles::class_name_of(context);
            e.depth = t_trace_depth;
            if (trace->capture_args && parms)
            {
                json args = json::object();
                for (FProperty* p : TFieldRange<FProperty>(function, EFieldIterationFlags::IncludeDeprecated))
                    if (p->HasAnyPropertyFlags(CPF_Parm)) args[to_utf8(p->GetName())] = property_to_json(p, parms, trace->opt, trace->opt.depth);
                e.args = args;
            }
            push_event(std::move(e));
        }

        void pe_pre(Hook::TCallbackIterationData<void>&, UObject* context, UFunction* function, void* parms)
        {
            trace_record("ProcessEvent", context, function, parms);
            ++t_trace_depth;
        }
        void pe_post(Hook::TCallbackIterationData<void>&, UObject*, UFunction*, void*)
        {
            if (t_trace_depth > 0) --t_trace_depth;
        }
        void script_hook_dispatch(UObject* context, FFrame& stack, void* result, bool is_post)
        {
            if (g_script_hooks.empty()) return;
            UFunction* fn = stack.Node();
            if (!fn) return;
            std::vector<std::shared_ptr<HookData>> matches;
            {
                std::lock_guard lock(g_hooks_mutex);
                for (auto& h : g_script_hooks)
                    if (h->function == fn && h->enabled) matches.push_back(h);
            }
            for (auto& h : matches)
            {
                UnrealScriptFunctionCallableContext ctx{context, stack, result};
                if (is_post) post_hook(ctx, h.get());
                else pre_hook(ctx, h.get());
            }
        }

        void pi_pre(Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack, void* result)
        {
            auto* fn = stack.Node();
            if (!fn) fn = stack.CurrentNativeFunction();
            trace_record("ProcessInternal", context, fn, nullptr);
            ++t_trace_depth;
            script_hook_dispatch(context, stack, result, false);
        }
        void pi_post(Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack, void* result)
        {
            if (t_trace_depth > 0) --t_trace_depth;
            script_hook_dispatch(context, stack, result, true);
        }
        // Script-to-script calls inside one Blueprint go through ProcessLocalScriptFunction, not ProcessInternal, so script hooks
        // must be dispatched from both.
        void plsf_pre(Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack, void* result)
        {
            auto* fn = stack.Node();
            if (!fn) fn = stack.CurrentNativeFunction();
            trace_record("ProcessLocalScriptFunction", context, fn, nullptr);
            ++t_trace_depth;
            script_hook_dispatch(context, stack, result, false);
        }
        void plsf_post(Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack, void* result)
        {
            if (t_trace_depth > 0) --t_trace_depth;
            script_hook_dispatch(context, stack, result, true);
        }

        void ensure_trace_hooks()
        {
            if (g_trace_hooks_installed.exchange(true)) return;
            Hook::RegisterProcessEventPreCallback(&pe_pre, {false, false, STR("UE5MCP"), STR("trace")});
            Hook::RegisterProcessEventPostCallback(&pe_post, {false, false, STR("UE5MCP"), STR("trace")});
            Hook::RegisterProcessInternalPreCallback(&pi_pre, {false, false, STR("UE5MCP"), STR("trace")});
            Hook::RegisterProcessInternalPostCallback(&pi_post, {false, false, STR("UE5MCP"), STR("trace")});
            if (Version::IsAbove(4, 21))
            {
                Hook::RegisterProcessLocalScriptFunctionPreCallback(&plsf_pre, {false, false, STR("UE5MCP"), STR("trace")});
                Hook::RegisterProcessLocalScriptFunctionPostCallback(&plsf_post, {false, false, STR("UE5MCP"), STR("trace")});
            }
            log("trace hooks installed (ProcessEvent/ProcessInternal/ProcessLocalScriptFunction)");
        }

        json events_since(std::uint64_t since, int limit, std::function<bool(const Event&)> pred)
        {
            json out = json::array();
            std::uint64_t next = since;
            std::lock_guard lock(g_ring_mutex);
            for (auto& e : g_ring)
            {
                if (e.seq <= since) continue;
                if (!pred(e)) { next = e.seq; continue; }
                out.push_back(event_json(e));
                next = e.seq;
                if (static_cast<int>(out.size()) >= limit) break;
            }
            return json{{"events", out}, {"next", next}, {"latest_seq", g_seq.load()}};
        }
    } // namespace

    void register_hook_tools(Registry& r)
    {
        r.add({"hook_add",
               "Hook a UFunction (pre and/or post). Every call is recorded in a ring buffer with caller, context, arguments, out params and return value; read it with hook_calls. Only calls routed through the UFunction are visible (Blueprint calls, ProcessEvent, RPCs), native-to-native calls are not.",
               obj({{"function", str("'Class:Func', full path '/Game/..BP_X_C:Func', or a bare name resolved on ref's class")}, {"ref", ref("Object whose class resolves a bare function name; also used as context_filter when context_filter=true")},
                    {"pre", boolean("Record before the call", true)}, {"post", boolean("Record after the call (outs + return)", true)},
                    {"capture_args", boolean("Serialize arguments", true)}, {"capture_return", boolean("Serialize return value", true)},
                    {"once", boolean("Disable after the first call (trace the next call)", false)}, {"context_filter", boolean("Only record calls where the context object is ref", false)},
                    {"depth", integer("Recursion depth for argument structs", 2)}, {"timeout_ms", timeout()}},
                   {"function"}),
               true},
              [](const json& p) {
                  auto spec = need<std::string>(p, "function");
                  UObject* ctx = p.contains("ref") && !p["ref"].is_null() ? Handles::instance().resolve(p["ref"].get<std::string>()) : nullptr;
                  auto* fn = find_function(spec, ctx);
                  if (!fn) throw RpcError(NotFound, "function not found: " + spec);
                  auto h = std::make_shared<HookData>();
                  h->function = fn;
                  h->function_path = Handles::path_of(fn);
                  h->capture_args = opt<bool>(p, "capture_args", true);
                  h->capture_return = opt<bool>(p, "capture_return", true);
                  h->once = opt<bool>(p, "once", false);
                  h->opt.depth = opt<int>(p, "depth", 2);
                  h->context_filter = opt<bool>(p, "context_filter", false) ? ctx : nullptr;
                  h->return_property = fn->GetReturnProperty();
                  {
                      std::lock_guard lock(g_hooks_mutex);
                      h->id = g_next_hook++;
                      g_hooks[h->id] = h;
                  }
                  const bool is_native = (static_cast<std::uint32_t>(fn->GetFunctionFlags()) & 0x400) != 0;
                  h->record_pre = opt<bool>(p, "pre", true);
                  h->record_post = opt<bool>(p, "post", true);
                  if (is_native)
                  {
                      if (h->record_pre)
                      {
                          h->pre_id = fn->RegisterPreHook(&pre_hook, h.get());
                          h->has_pre = true;
                      }
                      if (h->record_post)
                      {
                          h->post_id = fn->RegisterPostHook(&post_hook, h.get());
                          h->has_post = true;
                      }
                  }
                  else
                  {
                      ensure_trace_hooks();
                      h->script = true;
                      std::lock_guard lock(g_hooks_mutex);
                      g_script_hooks.push_back(h);
                  }
                  logf("hook %llu on %s", static_cast<unsigned long long>(h->id), h->function_path.c_str());
                  return json{{"hook_id", h->id}, {"function", h->function_path}, {"signature", describe_function(fn)}};
              });

        r.add({"hook_remove", "Remove a hook by id.", obj({{"hook_id", integer("From hook_add")}, {"timeout_ms", timeout()}}, {"hook_id"}), true}, [](const json& p) {
            auto id = need<std::uint64_t>(p, "hook_id");
            std::shared_ptr<HookData> h;
            {
                std::lock_guard lock(g_hooks_mutex);
                auto it = g_hooks.find(id);
                if (it == g_hooks.end()) throw RpcError(NotFound, "no hook " + std::to_string(id));
                h = it->second;
                g_hooks.erase(it);
            }
            h->enabled = false;
            if (h->has_pre) h->function->UnregisterHook(h->pre_id);
            if (h->has_post) h->function->UnregisterHook(h->post_id);
            if (h->script)
            {
                std::lock_guard lock(g_hooks_mutex);
                std::erase_if(g_script_hooks, [&](auto& s) { return s == h; });
            }
            // Keep the HookData alive a little: a callback in flight may still touch it. It leaks one small struct per removal
            // by design (held in a graveyard) rather than risk a use-after-free inside the engine.
            static std::vector<std::shared_ptr<HookData>> graveyard;
            graveyard.push_back(h);
            return json{{"ok", true}, {"calls", h->calls.load()}};
        });

        r.add({"hook_list", "List active hooks with call counts.", obj({}), false}, [](const json&) {
            json out = json::array();
            std::lock_guard lock(g_hooks_mutex);
            for (auto& [id, h] : g_hooks)
                out.push_back(json{{"hook_id", id}, {"function", h->function_path}, {"calls", h->calls.load()}, {"once", h->once}, {"enabled", h->enabled.load()}, {"pre", h->has_pre}, {"post", h->has_post}});
            return out;
        });

        r.add({"hook_calls", "Read recorded hook events from the ring buffer. Poll with since=<next> for new events. Order of seq is the real call order.",
               obj({{"hook_id", integer("Only this hook; default all hooks")}, {"since", integer("Return events with seq greater than this", 0)}, {"limit", integer("Max events", 200)},
                    {"function_glob", str("Glob on the function path")}, {"phase", str("pre | post; default both")}}),
               false},
              [](const json& p) {
                  std::uint64_t hook_id = opt<std::uint64_t>(p, "hook_id", 0);
                  std::string glob = opt<std::string>(p, "function_glob", "");
                  std::string phase = opt<std::string>(p, "phase", "");
                  return events_since(opt<std::uint64_t>(p, "since", 0), opt<int>(p, "limit", 200), [&](const Event& e) {
                      if (e.source != "hook") return false;
                      if (hook_id && e.hook_id != hook_id) return false;
                      if (!glob.empty() && !glob_match(glob, e.function)) return false;
                      if (!phase.empty() && e.phase != phase) return false;
                      return true;
                  });
              });

        r.add({"hook_clear", "Clear the ring buffer (hooks stay installed). With remove_all=true also remove every hook.",
               obj({{"remove_all", boolean("Also unregister all hooks", false)}, {"timeout_ms", timeout()}}), true},
              [](const json& p) {
                  int removed = 0;
                  if (opt<bool>(p, "remove_all", false))
                  {
                      std::lock_guard lock(g_hooks_mutex);
                      for (auto& [id, h] : g_hooks)
                      {
                          h->enabled = false;
                          if (h->has_pre) h->function->UnregisterHook(h->pre_id);
                          if (h->has_post) h->function->UnregisterHook(h->post_id);
                          ++removed;
                      }
                      g_script_hooks.clear();
                      static std::vector<std::shared_ptr<HookData>> graveyard;
                      for (auto& [id, h] : g_hooks) graveyard.push_back(h);
                      g_hooks.clear();
                  }
                  std::lock_guard lock(g_ring_mutex);
                  g_ring.clear();
                  return json{{"ok", true}, {"hooks_removed", removed}};
              });

        r.add({"trace_start",
               "Record every function call on the given classes (or matching a function glob) for a time window, from the engine's ProcessEvent, ProcessInternal and ProcessLocalScriptFunction paths. Catches script-to-script calls plain hooks miss. One trace runs at a time.",
               obj({{"classes", arr(str("class short name or path"), "Only calls whose context object is one of these classes (subclasses included)")},
                    {"function_glob", str("Glob on function name, e.g. *Spawn*")}, {"duration_ms", integer("Stop after this long", 5000)},
                    {"capture_args", boolean("Serialize ProcessEvent arguments (costly)", false)}, {"max_events", integer("Stop after this many events", 5000)},
                    {"sources", arr(str("ProcessEvent | ProcessInternal | ProcessLocalScriptFunction"), "Which engine paths to record")}, {"timeout_ms", timeout()}}),
               true},
              [](const json& p) {
                  ensure_trace_hooks();
                  auto t = std::make_shared<Trace>();
                  for (auto& c : opt<std::vector<std::string>>(p, "classes", {}))
                  {
                      auto* k = find_class(c);
                      if (!k) throw RpcError(NotFound, "class not found: " + c);
                      t->classes.insert(k);
                  }
                  t->function_glob = opt<std::string>(p, "function_glob", "");
                  t->deadline_ms = now_ms() + opt<int>(p, "duration_ms", 5000);
                  t->capture_args = opt<bool>(p, "capture_args", false);
                  t->max_events = opt<std::size_t>(p, "max_events", 5000);
                  for (auto& s : opt<std::vector<std::string>>(p, "sources", {"ProcessEvent", "ProcessInternal", "ProcessLocalScriptFunction"})) t->sources.insert(s);
                  t->first_seq = g_seq.load();
                  std::lock_guard lock(g_trace_mutex);
                  if (g_active_trace && g_active_trace->running) throw RpcError(InvalidParams, "trace " + std::to_string(g_active_trace->id) + " is still running; stop it first");
                  t->id = g_next_trace++;
                  g_traces[t->id] = t;
                  g_active_trace = t;
                  return json{{"trace_id", t->id}, {"deadline_ms", t->deadline_ms}};
              });

        r.add({"trace_stop", "Stop a trace and return everything it recorded.", obj({{"trace_id", integer("From trace_start")}}, {"trace_id"}), false}, [](const json& p) {
            auto id = need<std::uint64_t>(p, "trace_id");
            std::shared_ptr<Trace> t;
            {
                std::lock_guard lock(g_trace_mutex);
                auto it = g_traces.find(id);
                if (it == g_traces.end()) throw RpcError(NotFound, "no trace " + std::to_string(id));
                t = it->second;
                t->running = false;
                if (g_active_trace == t) g_active_trace.reset();
            }
            json r = events_since(t->first_seq, static_cast<int>(t->max_events), [&](const Event& e) { return e.trace_id == id; });
            r["count"] = t->count.load();
            r["truncated"] = t->count >= t->max_events;
            return r;
        });

        r.add({"trace_get", "Poll a running or finished trace.", obj({{"trace_id", integer("From trace_start")}, {"since", integer("seq cursor", 0)}, {"limit", integer("Max events", 500)}}, {"trace_id"}), false},
              [](const json& p) {
                  auto id = need<std::uint64_t>(p, "trace_id");
                  std::shared_ptr<Trace> t;
                  {
                      std::lock_guard lock(g_trace_mutex);
                      auto it = g_traces.find(id);
                      if (it == g_traces.end()) throw RpcError(NotFound, "no trace " + std::to_string(id));
                      t = it->second;
                  }
                  auto since = opt<std::uint64_t>(p, "since", 0);
                  json r = events_since(std::max(since, t->first_seq), opt<int>(p, "limit", 500), [&](const Event& e) { return e.trace_id == id; });
                  r["running"] = t->running.load() && now_ms() <= t->deadline_ms;
                  r["count"] = t->count.load();
                  return r;
              });
    }
} // namespace ue5mcp::bridge
