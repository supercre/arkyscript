// The Arky runtime for a host application. See arky_runtime.h.
//
// One translation unit: the Task and I/O modules are compiled into it, and the
// scheduler (async.lua) and the host bridge (host.lua) are carried as bytes.
#define ARKY_RUNTIME_STATIC
// The I/O module comes first: its platform headers must precede every other include.
#include "runtime/io/lua_io.cpp"
#include "runtime/task/lua_task.cpp"
#include "runtime/arky_runtime.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace arky::runtime {
namespace {
#include "runtime/arky_runtime_lua.inc"

// The runtime of a Lua state is a table in its registry under this key. Its values sit in
// integer slots, so the step of every frame needs no string lookup.
char state_key;
enum : int { slot_step = 1, slot_import, slot_deferred, slot_shutdown, slot_host, slot_task, slot_io, slot_environment,
             slot_count = slot_environment };
// A coroutine of the runtime's own, kept for the life of the Lua state. It is never suspended, so the
// host can always run a call on it: a deferred handle may be created inside an async function, whose
// own coroutine is suspended by the time the handle is resolved.
char thread_key;

// The host's callbacks live in a userdata of the Lua state and are destroyed with it.
struct host_state {
    explicit host_state(const options& host) : settings(host) {}

    options settings;
    module_source module;   // the module read last; see read_module
    // False once close() has ended this runtime. Deferred handles share it, as they may outlive the
    // runtime; the end of the Lua state they see through its lifetime token.
    std::shared_ptr<bool> open = std::make_shared<bool>(true);
};
char host_tag;
struct host_box {
    const void* tag;
    host_state* value;
};
constexpr const char* host_type = "arky.runtime.host";

int collect_host(lua_State* L) noexcept {
    if (lua_type(L, 1) != LUA_TUSERDATA || lua_objlen(L, 1) != sizeof(host_box)) return 0;
    auto* box = static_cast<host_box*>(lua_touserdata(L, 1));
    if (box->tag != &host_tag) return 0;
    delete std::exchange(box->value, nullptr);
    return 0;
}
host_state* host_of(lua_State* L) {
    return static_cast<host_box*>(lua_touserdata(L, lua_upvalueindex(1)))->value;
}

// The error value on top of the stack as text; pops it.
std::string failure(lua_State* L) {
    const char* text = lua_tostring(L, -1);
    std::string result = text ? text : "(error object is not a string)";
    lua_pop(L, 1);
    return result;
}

int upvalue(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

// The I/O module with the host's file reader.
int open_io(lua_State* L) {
    const auto& reader = host_of(L)->settings.file_reader;
    return io_module::open_for_host(L, reader ? &reader : nullptr);
}

// Calls the host's loader in a frame of its own (ARKY_NOINLINE). The strings it fills belong to
// the host object, so read_module can push them, which may raise a Lua memory error, with no
// C++ object on its own stack. 1: module read, 0: no such module, -1: message holds the failure.
ARKY_NOINLINE int read_module_call(host_state& host, const char* name, std::size_t size,
                                   char* message, std::size_t capacity) noexcept {
    try {
        host.module = module_source{};
        if (!host.settings.import_loader) return 0;
        if (!host.settings.import_loader(std::string_view(name, size), host.module)) return 0;
        if (host.module.name.empty()) host.module.name.assign(name, size);
        return 1;
    } catch (const std::exception& exception) {
        std::snprintf(message, capacity, "%s", exception.what());
    } catch (...) {
        std::snprintf(message, capacity, "%s", "unknown C++ exception");
    }
    return -1;
}

// host.lua's reader: read_module(name) -> chunk, chunk name | nil [, message]
int read_module(lua_State* L) {
    host_state* host = host_of(L);
    if (!host) {   // finalized: the Lua state is closing and one of its finalizers imports a module
        lua_pushnil(L);
        lua_pushliteral(L, "the Arky runtime is closing");
        return 2;
    }
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    std::size_t size = 0;
    const char* name = lua_tolstring(L, 1, &size);
    char message[512];
    const int found = read_module_call(*host, name, size, message, sizeof(message));
    if (found <= 0) {
        lua_pushnil(L);
        if (found == 0) return 1;
        lua_pushstring(L, message);
        return 2;
    }
    lua_pushlstring(L, host->module.chunk.data(), host->module.chunk.size());
    lua_pushlstring(L, host->module.name.data(), host->module.name.size());
    host->module = module_source{};
    return 2;
}

// package.loaded is on top of the stack. `require(name)` returns the module compiled into the
// runtime unless the host has already loaded something else under that name.
void publish(lua_State* L, const char* name, int module) {
    lua_getfield(L, -1, name);
    const bool vacant = lua_isnil(L, -1);
    lua_pop(L, 1);
    if (!vacant) return;
    lua_pushvalue(L, module);
    lua_setfield(L, -2, name);
}
void withdraw(lua_State* L, const char* name, int state, int slot) {
    lua_getfield(L, -1, name);
    lua_rawgeti(L, state, slot);
    const bool ours = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    if (!ours) return;
    lua_pushnil(L);
    lua_setfield(L, -2, name);
}

// Runs protected (lua_cpcall) behind detail::boundary: failures leave as Lua errors with a message,
// and open rethrows them as arky::error. Lua code is always called with lua_pcall here, so its
// errors never cross the boundary's catch.
int install(lua_State* L) {
    const auto* settings = static_cast<const options*>(lua_touserdata(L, 1));
    detail::reserve(L, 16);

    // The host object is empty until its finalizer is attached.
    if (luaL_newmetatable(L, host_type)) {
        lua_pushcfunction(L, collect_host);
        lua_setfield(L, -2, "__gc");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
    }
    auto* box = static_cast<host_box*>(lua_newuserdata(L, sizeof(host_box)));
    box->tag = &host_tag;
    box->value = nullptr;
    lua_pushvalue(L, -2);
    lua_setmetatable(L, -2);
    lua_remove(L, -2);
    const int host = lua_gettop(L);
    box->value = new host_state(*settings);

    // The native modules, compiled into this unit.
    lua_pushcfunction(L, luaopen_tynex_task);
    if (lua_pcall(L, 0, 1, 0) != 0) throw error("task module: " + failure(L));
    const int task = lua_gettop(L);
    if (!settings->task_jit) {
        lua_getfield(L, task, "_set_jit");
        lua_pushboolean(L, 0);
        if (lua_pcall(L, 1, 0, 0) != 0) throw error("task module: " + failure(L));
    }
    lua_pushvalue(L, host);
    lua_pushcclosure(L, open_io, 1);
    if (lua_pcall(L, 0, 1, 0) != 0) throw error("I/O module: " + failure(L));
    const int io = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    if (lua_istable(L, -1)) {
        publish(L, "tynex_task", task);
        publish(L, "tynex_io", io);
    }
    lua_pop(L, 1);

    // The scheduler receives loaders which hand it those same module instances.
    if (luaL_loadbuffer(L, reinterpret_cast<const char*>(async_lua), sizeof(async_lua), "=arky/async.lua") != 0)
        throw error("scheduler: " + failure(L));
    lua_pushvalue(L, task);
    lua_pushcclosure(L, upvalue, 1);
    lua_pushvalue(L, io);
    lua_pushcclosure(L, upvalue, 1);
    if (lua_pcall(L, 2, 1, 0) != 0) throw error("scheduler: " + failure(L));
    const int scheduler = lua_gettop(L);

    // The bridge owns the typed environment and the import cache.
    if (luaL_loadbuffer(L, reinterpret_cast<const char*>(host_lua), sizeof(host_lua), "=arky/host.lua") != 0)
        throw error("host bridge: " + failure(L));
    lua_pushvalue(L, scheduler);
    lua_pushvalue(L, host);
    lua_pushcclosure(L, read_module, 1);
    lua_pushboolean(L, !settings->import_loader);   // no loader: static imports use require
    if (lua_pcall(L, 3, 1, 0) != 0) throw error("host bridge: " + failure(L));
    const int bridge = lua_gettop(L);

    // An Arky module loaded by a plain require takes the typed environment from here.
    lua_createtable(L, 0, 1);
    lua_getfield(L, bridge, "environment");
    lua_setfield(L, -2, "environment");
    const int environment = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    if (lua_istable(L, -1)) publish(L, "tynex_runtime", environment);
    lua_pop(L, 1);

    lua_pushlightuserdata(L, &thread_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isthread(L, -1)) {
        lua_pushlightuserdata(L, &thread_key);
        lua_newthread(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_pop(L, 1);

    lua_createtable(L, slot_count, 0);
    lua_getfield(L, bridge, "step");
    lua_rawseti(L, -2, slot_step);
    lua_getfield(L, bridge, "import");
    lua_rawseti(L, -2, slot_import);
    lua_getfield(L, scheduler, "deferred");
    lua_rawseti(L, -2, slot_deferred);
    lua_getfield(L, task, "shutdown");
    lua_rawseti(L, -2, slot_shutdown);
    lua_pushvalue(L, host);
    lua_rawseti(L, -2, slot_host);
    lua_pushvalue(L, task);
    lua_rawseti(L, -2, slot_task);
    lua_pushvalue(L, io);
    lua_rawseti(L, -2, slot_io);
    lua_pushvalue(L, environment);
    lua_rawseti(L, -2, slot_environment);
    lua_pushlightuserdata(L, &state_key);
    lua_insert(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

// Pushes the runtime's state table, or pushes nothing and returns false when it is not open.
bool push_state(lua_State* L) {
    lua_pushlightuserdata(L, &state_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}
void require_state(lua_State* L) {
    if (!L) throw error("expected a live Lua state");
    detail::reserve(L, 6);
    if (!push_state(L)) throw error("the Arky runtime is not open in this Lua state");
}
bool run_step(lua_State* L, int max_resumes, const double* delta_seconds) {
    // Checked here, so that the message names what the host passed: the scheduler itself counts in
    // milliseconds, as scripts do.
    if (delta_seconds && !(*delta_seconds >= 0 && std::isfinite(*delta_seconds)))
        throw error("arky::runtime::step expects delta_seconds: a finite number, zero or more");
    require_state(L);
    lua_rawgeti(L, -1, slot_step);
    lua_pushinteger(L, max_resumes);
    if (delta_seconds) lua_pushnumber(L, *delta_seconds * 1000.0);
    if (lua_pcall(L, delta_seconds ? 2 : 1, 1, 0) != 0) {
        std::string message = failure(L);
        lua_pop(L, 1);
        throw error(message);
    }
    const bool pending = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    return pending;
}
} // namespace

void open(lua_State* L, const options& settings) {
    if (is_open(L)) throw error("the Arky runtime is already open in this Lua state");
    if (!L) throw error("expected a live Lua state");
    if (lua_cpcall(L, (detail::boundary<install>), const_cast<options*>(&settings)) != 0)
        throw error("cannot open the Arky runtime: " + failure(L));
}

bool is_open(lua_State* L) {
    if (!L || !lua_checkstack(L, 2) || !push_state(L)) return false;
    lua_pop(L, 1);
    return true;
}

bool step(lua_State* L, int max_resumes) {
    return run_step(L, max_resumes, nullptr);
}
bool step(lua_State* L, double delta_seconds, int max_resumes) {
    return run_step(L, max_resumes, &delta_seconds);
}

void import_module(lua_State* L, std::string_view name) {
    require_state(L);
    lua_rawgeti(L, -1, slot_import);
    lua_remove(L, -2);
    lua_pushlstring(L, name.data(), name.size());
    if (lua_pcall(L, 1, 1, 0) != 0) throw error(failure(L));
}

void close(lua_State* L) {
    // Closing must not throw: a host calls it while shutting down.
    if (!L || !lua_checkstack(L, 6) || !push_state(L)) return;
    const int state = lua_gettop(L);
    // Handles of this runtime's deferred Tasks do nothing from here on.
    lua_rawgeti(L, state, slot_host);
    if (auto* box = static_cast<host_box*>(lua_touserdata(L, -1)); box && box->value) *box->value->open = false;
    lua_pop(L, 1);
    // The pool joins its workers here. Without it they stay until the Lua state is closed.
    lua_rawgeti(L, state, slot_shutdown);
    if (lua_pcall(L, 0, 0, 0) != 0) lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    if (lua_istable(L, -1)) {
        withdraw(L, "tynex_task", state, slot_task);
        withdraw(L, "tynex_io", state, slot_io);
        withdraw(L, "tynex_runtime", state, slot_environment);
    }
    lua_pushlightuserdata(L, &state_key);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_settop(L, state - 1);
}

// The resolve and reject functions of one scheduler.deferred() call, shared by the copies of a handle.
struct deferred::shared {
    std::shared_ptr<detail::vm_lifetime> lifetime;   // the Lua state: the references below live in its registry
    std::shared_ptr<const bool> open;                // the runtime which owns the Task
    lua_State* thread = nullptr;
    int resolve = LUA_NOREF;
    int reject = LUA_NOREF;
    ~shared() {
        if (!lifetime || !lifetime->alive || !lua_checkstack(thread, 2)) return;
        luaL_unref(thread, LUA_REGISTRYINDEX, resolve);
        luaL_unref(thread, LUA_REGISTRYINDEX, reject);
    }
};

deferred::deferred(lua_State* L) {
    auto state = std::make_shared<shared>();
    require_state(L);
    const int runtime = lua_gettop(L);
    try {
        // A closing Lua state finalizes the host object while the registry still holds the
        // runtime: a finalizer which runs after that gets here.
        lua_rawgeti(L, runtime, slot_host);
        const auto* box = static_cast<const host_box*>(lua_touserdata(L, -1));
        if (!box || !box->value) throw error("the Arky runtime is closing");
        state->open = box->value->open;
        state->lifetime = detail::lifetime_of(L);
        lua_pushlightuserdata(L, &thread_key);
        lua_rawget(L, LUA_REGISTRYINDEX);
        state->thread = lua_tothread(L, -1);
        lua_rawgeti(L, runtime, slot_deferred);
        if (lua_pcall(L, 0, 3, 0) != 0) throw error(failure(L));
    } catch (...) {
        lua_settop(L, runtime - 1);
        throw;
    }
    state->reject = luaL_ref(L, LUA_REGISTRYINDEX);
    state->resolve = luaL_ref(L, LUA_REGISTRYINDEX);
    // The Task takes the place of the runtime's table: it is all this leaves on the stack.
    lua_replace(L, runtime);
    lua_settop(L, runtime);
    state_ = std::move(state);
}

lua_State* deferred::call_state() const noexcept {
    return state_ && state_->lifetime->alive && *state_->open ? state_->thread : nullptr;
}
void deferred::push_function(lua_State* L, bool resolve) const {
    lua_rawgeti(L, LUA_REGISTRYINDEX, resolve ? state_->resolve : state_->reject);
}
bool deferred::complete(lua_State* L, int values) const {
    if (lua_pcall(L, values, 1, 0) != 0) throw error(failure(L));
    return lua_toboolean(L, -1) != 0;
}
bool deferred::reject(std::string_view message) const {
    lua_State* L = call_state();
    if (!L) return false;
    detail::stack_guard stack(L);
    detail::reserve(L, 4);
    push_function(L, false);
    lua_pushlstring(L, message.data(), message.size());
    return complete(L, 1);
}

} // namespace arky::runtime
