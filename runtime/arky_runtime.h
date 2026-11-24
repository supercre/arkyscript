#pragma once

// The Arky runtime for a host application: the scheduler behind async/await,
// worker tasks, async file/TCP I/O and static imports, installed into one Lua
// state. The host compiles runtime/arky_runtime.cpp (one translation unit, with
// the repository root on the include path) and links its own LuaJIT; no shared
// module is loaded at run time.
//
// Every Lua state has a runtime of its own, kept in that state's registry and
// released with it. Nothing is shared between states, so a host may run several
// (tools, tests, a restarted game VM); each runtime is used only on the thread
// which owns its Lua state.
//
//     arky::runtime::options settings;
//     settings.import_loader = [](std::string_view name, arky::runtime::module_source& module) {
//         return read_generated_lua(name, module.chunk, module.name);
//     };
//     arky::runtime::open(L, settings);
//     arky::runtime::import_module(L, "game.main");   // runs the entry module
//     lua_pop(L, 1);
//     ...
//     arky::runtime::step(L, delta_seconds);          // once per frame
//
// Functions report failures by throwing arky::error; they never raise a Lua
// error through the caller.

#include "binder/lua_object.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace arky::runtime {

// What a host returns for a static import.
struct module_source {
    std::string chunk;   // generated Lua: source text or LuaJIT bytecode
    std::string name;    // shown in error messages and tracebacks; the module name when left empty
};

struct options {
    // Static imports (`import x from "name"`). Fills `module` and returns true, or returns
    // false when the host has no module of that name. Runs on the Lua thread.
    // Without a loader, static imports use the Lua state's require (and its package.loaded),
    // as a game engine with its own module loaders wants: legacy require and Arky imports then
    // share every module, and an Arky module loaded by require takes the runtime's environment
    // itself from package.loaded.tynex_runtime.
    std::function<bool(std::string_view name, module_source& module)> import_loader;

    // Source of fs.read_file, for hosts which keep files in archives. Runs on a worker thread of
    // the runtime, never on the Lua thread, and may run for several reads at once. Fills `bytes`
    // with at most max_bytes and returns true, or returns false with a message in `error`.
    // Without it fs.read_file reads regular files of the operating system.
    std::function<bool(std::string_view path, std::size_t max_bytes, std::string& bytes, std::string& error)> file_reader;

    // LuaJIT's JIT compiler in the Lua states which run worker tasks. Every task runs in a fresh
    // state of its own; false turns the JIT off there (luaJIT_setmode), for a host which runs its
    // own Lua state interpreted and wants its tasks to match.
    bool task_jit = true;
};

// Installs the runtime into L. One runtime per Lua state: opening twice is an error.
// `require("tynex_task")` and `require("tynex_io")` return the modules compiled into the runtime;
// package.loaded.tynex_runtime holds the environment of Arky modules (for generated code).
void open(lua_State* L, const options& settings = {});

// True between open and close (or until the Lua state is closed).
bool is_open(lua_State* L);

// One step of the scheduler: completes the timers which are due, collects finished native work and
// resumes async functions which are ready, at most max_resumes of them. Call it once per frame; it
// never blocks. Returns true while tasks are still pending.
//
// scheduler.sleep waits on host time, which advances by the delta_seconds of each step: a game
// passes its frame time, so pausing or scaling the game applies to those timers. Without
// delta_seconds host time follows real time. scheduler.sleep_realtime always waits on real time.
// The host passes seconds, as engines count frame time; scripts give their timers milliseconds.
bool step(lua_State* L, int max_resumes = 64);
bool step(lua_State* L, double delta_seconds, int max_resumes = 64);

// Loads the module through options::import_loader, as `import` does, and pushes its value.
// A module is run once; later imports of the same name push the same value.
void import_module(lua_State* L, std::string_view name);

// Stops the worker pool and forgets the runtime. Optional: closing the Lua state releases
// everything too. Tasks which were still pending never complete, and deferred handles of this
// runtime are dead from then on.
void close(lua_State* L);

// The host's side of scheduler.deferred(): a Task which the host completes later, when a
// response arrives or an animation ends. A bound function creates one, returns the Task to the
// script and keeps the handle:
//
//     arky::runtime::deferred done(L);              // the Task is now on L's stack
//     request(url, [done](const std::string& body) { done.resolve(body); });
//     return 1;
//
// The Task may also be completed at once, inside the bound function which created it.
//
// Copies share the Task. A handle keeps neither the runtime nor the Lua state alive: once the
// runtime is closed (close, or the end of its Lua state), resolve and reject do nothing, so a late
// callback is harmless. Use and destroy handles on the thread which owns the Lua state.
class deferred {
public:
    deferred() = default;
    // Creates the Task and pushes it onto L's stack.
    explicit deferred(lua_State* L);

    // Completes the Task with these values, converted as results of bound functions are. Returns
    // true if this call completed it, and false if it was already completed, failed or cancelled,
    // or if its runtime is closed.
    template<class... A> bool resolve(A&&... values) const {
        lua_State* L = call_state();
        if (!L) return false;
        detail::stack_guard stack(L);
        detail::reserve(L, static_cast<int>(sizeof...(A)) + 4);
        push_function(L, true);
        (detail::push(L, std::forward<A>(values)), ...);
        return complete(L, static_cast<int>(sizeof...(A)));
    }
    // Fails the Task: code which awaits it receives `message` as the error. Returns as resolve does.
    bool reject(std::string_view message) const;
    // True while the Task's runtime is open. An empty handle is not alive.
    bool alive() const noexcept { return call_state() != nullptr; }

private:
    struct shared;
    lua_State* call_state() const noexcept;
    void push_function(lua_State* L, bool resolve) const;
    bool complete(lua_State* L, int values) const;
    std::shared_ptr<shared> state_;
};

} // namespace arky::runtime
