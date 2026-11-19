#pragma once

// Runtime-named native classes for raw (generated or legacy) bindings.
// A class is identified by a host-defined name instead of a C++ type, members
// are Lua C functions, and objects are untyped native pointers.
#include "lua_cclosure.h"
#include <cstddef>
#include <cstring>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>

// Typed pushes pick the registered class of the dynamic type when RTTI exists.
#if defined(__cpp_rtti) || defined(__GXX_RTTI) || defined(_CPPRTTI)
#define ARKY_BINDER_RTTI 1
#else
#define ARKY_BINDER_RTTI 0
#endif

namespace arky::detail {
// The pointer comes first so a wrapper can be read as a boxed native pointer.
struct named_storage {
    void* value = nullptr;
    bool owned = false; // Lua runs the class collector when the wrapper is collected.
};
static_assert(std::is_standard_layout_v<named_storage> && offsetof(named_storage, value) == 0);

inline char named_registry_key;  // registry: class name -> member table
inline char named_pins_key;      // cache metatable: native pointer -> wrapper (strong)
inline char named_name_key;      // member table: class name; also marks a named class
inline char named_table_key;     // member table: public class table
inline char named_cache_key;     // member table: native pointer -> wrapper (weak values)
inline char named_kinds_key;     // member table: every class name this class is a kind of
inline char named_getters_key;
inline char named_setters_key;
inline char named_index_get_key;
inline char named_index_set_key;
inline char named_collector_key;
inline char named_natives_key;   // member table: values a script replaced through the class table
inline char named_operators_key; // member table: operators registered on this class
inline char named_derived_key;   // member table: true once another class names it as its base
inline char named_guard_key;     // member table: whether expired objects refuse native calls
inline char named_bound_key;     // member table: type keys of the C++ types bound to this class
inline char named_lifetime_key;  // member table: how wrappers of natively owned objects are kept
inline char named_dynamic_key;   // registry: dynamic C++ type -> member table
// Identifies the named class bound to C++ type T: a registry key for its member
// table and an entry in the kinds of every class derived from it.
template<class T> inline char named_type_key;
struct named_lifetime {
    bool pin = false;
    void (*exposed)(void*) noexcept = nullptr;
};
inline constexpr const char* const named_operator_events[] = {
    "__add", "__sub", "__mul", "__div", "__mod", "__pow", "__unm",
    "__concat", "__len", "__lt", "__le", "__eq", "__call"};

// A member table has its base as metatable, so every access below is raw.
// These helpers take absolute stack indices.
inline void named_get(lua_State* L, int table, char* key) {
    lua_pushlightuserdata(L, key);
    lua_rawget(L, table);
}
inline void named_set(lua_State* L, int table, char* key) {
    lua_pushlightuserdata(L, key);
    lua_insert(L, -2);
    lua_rawset(L, table);
}
inline void named_set(lua_State* L, int table, const char* name) {
    lua_pushstring(L, name);
    lua_insert(L, -2);
    lua_rawset(L, table);
}

// Pushes exactly one value: the member table, or nil when the class is unknown.
inline bool push_named_members(lua_State* L, std::string_view name) {
    lua_pushlightuserdata(L, &named_registry_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) return false;
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, -2);
    lua_remove(L, -2);
    return lua_istable(L, -1);
}
// Pins belong to a hierarchy like its identity cache: the strong table lives
// in the cache's metatable. Pushes exactly one value, the pin table or nil.
inline bool push_named_pins(lua_State* L, int cache, bool create) {
    if (!lua_istable(L, cache) || !lua_getmetatable(L, cache)) { lua_pushnil(L); return false; }
    lua_pushlightuserdata(L, &named_pins_key);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1) && create) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushlightuserdata(L, &named_pins_key);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_remove(L, -2);
    return lua_istable(L, -1);
}
// Clears table[pointer] only when present, which never allocates.
inline void named_forget(lua_State* L, int table, void* pointer) {
    lua_pushlightuserdata(L, pointer);
    lua_rawget(L, table);
    const bool present = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (!present) return;
    lua_pushlightuserdata(L, pointer);
    lua_pushnil(L);
    lua_rawset(L, table);
}
inline bool named_derives(lua_State* L, int derived, int base) {
    lua_pushvalue(L, derived);
    while (!lua_rawequal(L, -1, base)) {
        if (!lua_getmetatable(L, -1)) { lua_pop(L, 1); return false; }
        lua_remove(L, -2);
    }
    lua_pop(L, 1);
    return true;
}
// Recognizes a wrapper by its size and class marker. Never raises.
inline named_storage* named_box(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TUSERDATA || lua_objlen(L, index) != sizeof(named_storage)
        || !lua_checkstack(L, 2) || !lua_getmetatable(L, index)) return nullptr;
    lua_pushlightuserdata(L, &named_name_key);
    lua_rawget(L, -2);
    const bool named = lua_type(L, -1) == LUA_TSTRING;
    lua_pop(L, 2);
    return named ? static_cast<named_storage*>(lua_touserdata(L, index)) : nullptr;
}

// The receiver of an object metamethod is a wrapper unless a script moved the
// metatable to another userdata; the size check keeps that case memory-safe.
inline bool named_expired(lua_State* L) {
    return lua_objlen(L, 1) == sizeof(named_storage) && !static_cast<named_storage*>(lua_touserdata(L, 1))->value;
}
// The nearest class with an explicit setting decides. Only expired objects ask.
inline bool named_guarded(lua_State* L) {
    bool guarded = false;
    lua_pushvalue(L, lua_upvalueindex(1));
    while (true) {
        lua_pushlightuserdata(L, &named_guard_key);
        lua_rawget(L, -2);
        const bool configured = !lua_isnil(L, -1);
        if (configured) guarded = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (configured || !lua_getmetatable(L, -1)) break;
        lua_remove(L, -2);
    }
    lua_pop(L, 1);
    return guarded;
}
inline int named_expired_call(lua_State* L) { return luaL_error(L, "expired native object"); }

// Dispatchers follow the Lua C API convention and hold no C++ resources.
inline int named_index(lua_State* L) {
    const bool instance = lua_type(L, 1) == LUA_TUSERDATA;
    const bool expired = instance && named_expired(L);
    if (instance) {
        // Instance fields shadow class members; the field table's metamethods apply.
        lua_getfenv(L, 1);
        if (!lua_rawequal(L, -1, LUA_REGISTRYINDEX)) {
            lua_pushvalue(L, 2);
            lua_gettable(L, -2);
            if (!lua_isnil(L, -1)) return 1;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    } else if (lua_type(L, 1) != LUA_TTABLE) {
        lua_pushnil(L);
        return 1;
    }
    // A class table reads its own class only, like a typed class factory. Any
    // other table is a member table reached through its base: plain inheritance.
    bool class_table = false;
    if (!instance) {
        lua_pushlightuserdata(L, &named_table_key);
        lua_rawget(L, lua_upvalueindex(1));
        class_table = lua_rawequal(L, -1, 1) != 0;
        lua_pop(L, 1);
    }
    const bool numeric = instance && lua_type(L, 2) == LUA_TNUMBER;
    lua_pushvalue(L, lua_upvalueindex(1));
    while (true) {
        if (numeric) {
            lua_pushlightuserdata(L, &named_index_get_key);
            lua_rawget(L, -2);
        } else {
            lua_pushvalue(L, 2);
            lua_rawget(L, -2);
            if (!lua_isnil(L, -1)) {
                // A guarded class hands out a raising function in place of
                // native code that would run with a dead receiver.
                if (expired && lua_iscfunction(L, -1) && named_guarded(L)) lua_pushcfunction(L, named_expired_call);
                return 1;
            }
            lua_pop(L, 1);
            if (!instance && !class_table) lua_pushnil(L);
            else {
                lua_pushlightuserdata(L, &named_getters_key);
                lua_rawget(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushvalue(L, 2);
                    lua_rawget(L, -2);
                    lua_remove(L, -2);
                }
            }
        }
        if (!lua_isnil(L, -1)) {
            if (expired && named_guarded(L)) return named_expired_call(L);
            lua_pushvalue(L, 1);
            lua_pushvalue(L, 2);
            lua_call(L, 2, 1);
            return 1;
        }
        lua_pop(L, 1);
        if (class_table || !lua_getmetatable(L, -1)) break;
        lua_remove(L, -2);
    }
    lua_pushnil(L);
    return 1;
}
// Remembers the first value a script replaces through a class table.
inline void named_backup(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    while (true) {
        const int current = lua_gettop(L);
        named_get(L, current, &named_natives_key);
        if (lua_istable(L, -1)) {
            lua_pushvalue(L, 2);
            lua_rawget(L, -2);
            if (!lua_isnil(L, -1)) break;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        lua_pushvalue(L, 2);
        lua_rawget(L, current);
        if (!lua_isnil(L, -1)) {
            named_get(L, current, &named_natives_key);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                lua_newtable(L);
                lua_pushvalue(L, -1);
                named_set(L, current, &named_natives_key);
            }
            lua_pushvalue(L, 2);
            lua_pushvalue(L, -3);
            lua_rawset(L, -3);
            break;
        }
        lua_pop(L, 1);
        if (!lua_getmetatable(L, current)) break;
        lua_remove(L, current);
    }
    lua_settop(L, 3);
}
inline int named_newindex(lua_State* L) {
    const int kind = lua_type(L, 1);
    if (kind != LUA_TUSERDATA && kind != LUA_TTABLE) return luaL_error(L, "expected a native object or class");
    lua_settop(L, 3);
    const bool instance = kind == LUA_TUSERDATA;
    if (!instance) {
        lua_pushlightuserdata(L, &named_table_key);
        lua_rawget(L, lua_upvalueindex(1));
        const bool class_table = lua_rawequal(L, -1, 1) != 0;
        lua_pop(L, 1);
        // A member table reached through its base stores into itself.
        if (!class_table) { lua_rawset(L, 1); return 0; }
    }
    const bool numeric = instance && lua_type(L, 2) == LUA_TNUMBER;
    lua_pushvalue(L, lua_upvalueindex(1));
    while (true) {
        lua_pushlightuserdata(L, numeric ? &named_index_set_key : &named_setters_key);
        lua_rawget(L, -2);
        if (!numeric && lua_istable(L, -1)) {
            lua_pushvalue(L, 2);
            lua_rawget(L, -2);
            lua_remove(L, -2);
        }
        if (!lua_isnil(L, -1)) {
            if (instance && named_expired(L) && named_guarded(L)) return named_expired_call(L);
            lua_pushvalue(L, 1);
            if (numeric) lua_pushvalue(L, 2);
            lua_pushvalue(L, 3);
            lua_call(L, numeric ? 3 : 2, 0);
            return 0;
        }
        lua_pop(L, 1);
        if (!instance || !lua_getmetatable(L, -1)) break; // a class table reaches its own setters only
        lua_remove(L, -2);
    }
    lua_settop(L, 3);
    if (instance) {
        lua_getfenv(L, 1);
        if (lua_rawequal(L, -1, LUA_REGISTRYINDEX)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfenv(L, 1);
        }
        lua_insert(L, 2);
        lua_settable(L, 2);
        return 0;
    }
    named_backup(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 2);
    lua_rawset(L, 2);
    return 0;
}
inline int named_collect(lua_State* L) {
    auto* box = named_box(L, 1);
    if (!box) return 0;
    int status = 0;
    if (box->value && box->owned) {
        // The nearest class with a collector releases a Lua-owned object.
        lua_getmetatable(L, 1);
        while (true) {
            lua_pushlightuserdata(L, &named_collector_key);
            lua_rawget(L, -2);
            if (!lua_isnil(L, -1)) break;
            lua_pop(L, 1);
            if (!lua_getmetatable(L, -1)) { lua_pushnil(L); break; }
            lua_remove(L, -2);
        }
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1);
            status = lua_pcall(L, 1, 0, 0);
        }
    }
    *box = named_storage{};
    return status ? lua_error(L) : 0;
}
// Class tables share the member table as metatable; operators apply to objects.
inline int named_operator(lua_State* L) {
    if (lua_type(L, 1) != LUA_TUSERDATA && lua_type(L, 2) != LUA_TUSERDATA) {
        if (!lua_toboolean(L, lua_upvalueindex(2))) return luaL_error(L, "native operator expects an object");
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

// Pushes the member table, creating the class on first use.
inline void named_create(lua_State* L, std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos)
        throw error("expected a non-empty native class name");
    if (push_named_members(L, name)) return;
    lua_pop(L, 1);
    lua_pushlightuserdata(L, &named_registry_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushlightuserdata(L, &named_registry_key);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    const int names = lua_gettop(L);
    lua_newtable(L);
    const int members = lua_gettop(L);
    lua_pushlstring(L, name.data(), name.size());
    named_set(L, members, &named_name_key);
    lua_newtable(L); // identity cache; a base registration replaces it with the root's
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    named_set(L, members, &named_cache_key);
    lua_newtable(L);
    lua_pushlstring(L, name.data(), name.size());
    lua_pushboolean(L, true);
    lua_rawset(L, -3);
    named_set(L, members, &named_kinds_key);
    lua_pushvalue(L, members);
    lua_pushcclosure(L, named_index, 1);
    named_set(L, members, "__index");
    lua_pushvalue(L, members);
    lua_pushcclosure(L, named_newindex, 1);
    named_set(L, members, "__newindex");
    lua_pushcfunction(L, named_collect);
    named_set(L, members, "__gc");
    lua_pushlstring(L, name.data(), name.size());
    lua_pushvalue(L, members);
    lua_rawset(L, names);
    lua_remove(L, names);
}
inline bool named_has_operators(lua_State* L) {
    lua_pushlightuserdata(L, &named_registry_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    bool used = false;
    if (lua_istable(L, -1)) {
        lua_pushlightuserdata(L, &named_operators_key);
        lua_rawget(L, -2);
        used = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return used;
}
// Lua reads metamethods raw, so each class carries the nearest registered
// handler. Every class gets its own function object: Lua then applies
// comparison operators only between objects of the same class.
inline void named_refresh_operators(lua_State* L, int cls) {
    for (const char* event : named_operator_events) {
        lua_pushvalue(L, cls);
        while (true) {
            const int current = lua_gettop(L);
            named_get(L, current, &named_operators_key);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, event);
                lua_rawget(L, -2);
                lua_remove(L, -2);
            }
            if (!lua_isnil(L, -1)) break;
            lua_pop(L, 1);
            if (!lua_getmetatable(L, current)) { lua_pushnil(L); break; }
            lua_remove(L, current);
        }
        if (!lua_isnil(L, -1) && std::strcmp(event, "__call") != 0) {
            lua_pushboolean(L, std::strcmp(event, "__eq") == 0);
            lua_pushcclosure(L, named_operator, 2);
        }
        named_set(L, cls, event);
        lua_pop(L, 1);
    }
}
// Recomputes what a class takes from its base chain: the hierarchy's identity
// cache, every class name it is a kind of, and inherited operators.
inline void named_inherit(lua_State* L, int cls) {
    stack_guard stack(L);
    reserve(L, 12);
    lua_newtable(L);
    const int kinds = lua_gettop(L);
    lua_pushvalue(L, cls);
    while (true) {
        const int current = lua_gettop(L);
        named_get(L, current, &named_name_key);
        lua_pushboolean(L, true);
        lua_rawset(L, kinds);
        named_get(L, current, &named_bound_key);
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                lua_pop(L, 1);
                lua_pushvalue(L, -1);
                lua_pushboolean(L, true);
                lua_rawset(L, kinds);
            }
        }
        lua_pop(L, 1);
        if (!lua_getmetatable(L, current)) break;
        named_get(L, current + 1, &named_name_key);
        const bool named = lua_type(L, -1) == LUA_TSTRING;
        lua_pop(L, 1);
        if (!named) { lua_pop(L, 1); break; }
        lua_remove(L, current);
    }
    named_get(L, lua_gettop(L), &named_cache_key);
    named_set(L, cls, &named_cache_key);
    lua_pushvalue(L, kinds);
    named_set(L, cls, &named_kinds_key);
    if (named_has_operators(L)) named_refresh_operators(L, cls);
}
template<class F> void named_each_derived(lua_State* L, int cls, F&& action) {
    action(cls);
    named_get(L, cls, &named_derived_key);
    const bool derived = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (!derived) return;
    lua_pushlightuserdata(L, &named_registry_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const int names = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, names)) {
        const int candidate = lua_gettop(L);
        if (lua_istable(L, candidate) && !lua_rawequal(L, candidate, cls) && named_derives(L, candidate, cls))
            action(candidate);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

// The nearest class that configures native lifetime decides how a new wrapper
// is kept. Runs only when a wrapper is created.
inline void named_apply_lifetime(lua_State* L, int members, int wrapper, void* pointer) {
    lua_pushvalue(L, members);
    while (true) {
        lua_pushlightuserdata(L, &named_lifetime_key);
        lua_rawget(L, -2);
        if (lua_type(L, -1) == LUA_TUSERDATA && lua_objlen(L, -1) == sizeof(named_lifetime)) break;
        lua_pop(L, 1);
        if (!lua_getmetatable(L, -1)) { lua_pop(L, 1); return; }
        lua_remove(L, -2);
    }
    const named_lifetime policy = *static_cast<named_lifetime*>(lua_touserdata(L, -1));
    lua_pop(L, 2);
    if (policy.exposed) policy.exposed(pointer);
    if (!policy.pin) return;
    named_get(L, members, &named_cache_key);
    if (push_named_pins(L, lua_gettop(L), true)) {
        lua_pushlightuserdata(L, pointer);
        lua_pushvalue(L, wrapper);
        lua_rawset(L, -3);
    }
    lua_pop(L, 2);
}
// Replaces the member table at absolute index members with the wrapper for
// pointer: the cached one, refined to a more specific class, or a new one.
inline bool named_push_at(lua_State* L, void* pointer, int members) {
    named_get(L, members, &named_cache_key);
    const int cache = members + 1;
    if (!lua_istable(L, cache)) { lua_settop(L, members - 1); lua_pushnil(L); return false; }
    lua_pushlightuserdata(L, pointer);
    lua_rawget(L, cache);
    const auto* box = named_box(L, -1);
    if (box && box->value == pointer) {
        lua_getmetatable(L, -1);
        const bool keep = named_derives(L, lua_gettop(L), members);
        lua_pop(L, 1);
        if (!keep) { lua_pushvalue(L, members); lua_setmetatable(L, -2); }
    } else {
        lua_pop(L, 1);
        ::new (lua_newuserdata(L, sizeof(named_storage))) named_storage{pointer, false};
        lua_pushvalue(L, members);
        lua_setmetatable(L, -2);
        lua_pushvalue(L, LUA_REGISTRYINDEX); // marks "no instance fields"
        lua_setfenv(L, -2);
        lua_pushlightuserdata(L, pointer);
        lua_pushvalue(L, -2);
        lua_rawset(L, cache);
        named_apply_lifetime(L, members, lua_gettop(L), pointer);
    }
    lua_replace(L, members);
    lua_settop(L, members);
    return true;
}

// Classes bound to a C++ type. A typed argument is accepted when the object's
// class has the type among its kinds; no name is hashed on this path.
enum class named_match { no, expired, yes };
template<class C> named_match named_bound(lua_State* L, int index, C*& out) {
    if (lua_type(L, index) != LUA_TUSERDATA || lua_objlen(L, index) != sizeof(named_storage)
        || !lua_checkstack(L, 3) || !lua_getmetatable(L, index)) return named_match::no;
    lua_pushlightuserdata(L, &named_kinds_key);
    lua_rawget(L, -2);
    bool kind = false;
    if (lua_istable(L, -1)) {
        lua_pushlightuserdata(L, &named_type_key<C>);
        lua_rawget(L, -2);
        kind = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    if (!kind) return named_match::no;
    out = static_cast<C*>(static_cast<named_storage*>(lua_touserdata(L, index))->value);
    return out ? named_match::yes : named_match::expired;
}
template<class C> bool push_named_bound(lua_State* L) {
    lua_pushlightuserdata(L, &named_type_key<C>);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}
#if ARKY_BINDER_RTTI
// One per VM: the member table of each bound polymorphic type, so a base
// pointer is exposed as the registered class of the object it points to.
using named_type_resolver = const char* (*)(lua_State*, const std::type_info&) noexcept;
struct named_dynamic_types {
    std::unordered_map<std::type_index, int> slots;
    named_type_resolver resolver = nullptr;
};
inline int collect_named_dynamic(lua_State* L) noexcept {
    if (lua_type(L, 1) == LUA_TUSERDATA && lua_objlen(L, 1) == sizeof(named_dynamic_types))
        static_cast<named_dynamic_types*>(lua_touserdata(L, 1))->~named_dynamic_types();
    return 0;
}
// Pushes the VM's record, creating it on first use.
inline named_dynamic_types& push_named_dynamic_types(lua_State* L) {
    lua_pushlightuserdata(L, &named_dynamic_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) != LUA_TUSERDATA) {
        lua_pop(L, 1);
        ::new (lua_newuserdata(L, sizeof(named_dynamic_types))) named_dynamic_types{};
        lua_newtable(L);
        lua_pushcfunction(L, collect_named_dynamic);
        lua_setfield(L, -2, "__gc");
        lua_setmetatable(L, -2);
        lua_newtable(L);
        lua_setfenv(L, -2);
        lua_pushlightuserdata(L, &named_dynamic_key);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    return *static_cast<named_dynamic_types*>(lua_touserdata(L, -1));
}
inline void named_register_dynamic(lua_State* L, const std::type_info& type, int members) {
    stack_guard stack(L);
    reserve(L, 8);
    auto& slots = push_named_dynamic_types(L).slots;
    lua_getfenv(L, -1);
    const auto found = slots.find(std::type_index(type));
    const int slot = found != slots.end() ? found->second : static_cast<int>(lua_objlen(L, -1)) + 1;
    lua_pushvalue(L, members);
    lua_rawseti(L, -2, slot);
    slots[std::type_index(type)] = slot;
}
inline bool push_named_dynamic(lua_State* L, const std::type_info& type) {
    lua_pushlightuserdata(L, &named_dynamic_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_type(L, -1) != LUA_TUSERDATA) { lua_pop(L, 1); return false; }
    const auto& types = *static_cast<named_dynamic_types*>(lua_touserdata(L, -1));
    const auto& slots = types.slots;
    const auto found = slots.find(std::type_index(type));
    if (found == slots.end()) {
        // A type bound by name only: the host says which class it belongs to.
        const auto resolver = types.resolver;
        lua_pop(L, 1);
        const char* name = resolver ? resolver(L, type) : nullptr;
        if (!name) return false;
        if (push_named_members(L, name)) return true;
        lua_pop(L, 1);
        return false;
    }
    lua_getfenv(L, -1);
    lua_rawgeti(L, -1, found->second);
    lua_replace(L, -3);
    lua_pop(L, 1);
    if (lua_istable(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}
#endif
// Lua ownership: the collector releases the object when its wrapper is
// collected, so the wrapper must not stay pinned as a natively owned one.
inline bool named_take_ownership(lua_State* L, int index) {
    auto* box = named_box(L, index);
    if (!box || !box->value || box->owned) return false;
    box->owned = true;
    if (!lua_checkstack(L, 6)) return true;
    const int top = lua_gettop(L);
    index = absolute_index(L, index);
    if (lua_getmetatable(L, index)) {
        named_get(L, top + 1, &named_cache_key);
        if (push_named_pins(L, top + 2, false)) named_forget(L, top + 3, box->value);
    }
    lua_settop(L, top);
    return true;
}
template<class T> void named_bind(lua_State* L, int members) {
    static_assert(std::is_class_v<T> && !std::is_const_v<T>);
    stack_guard stack(L);
    reserve(L, 12);
    lua_pushlightuserdata(L, &named_type_key<T>);
    lua_pushvalue(L, members);
    lua_rawset(L, LUA_REGISTRYINDEX);
    // A class can stand for several C++ types (a host subclass exposed under its base's name).
    named_get(L, members, &named_bound_key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        named_set(L, members, &named_bound_key);
    }
    lua_pushlightuserdata(L, &named_type_key<T>);
    lua_pushboolean(L, true);
    lua_rawset(L, -3);
    lua_pop(L, 1);
#if ARKY_BINDER_RTTI
    if constexpr (std::is_polymorphic_v<T>) named_register_dynamic(L, typeid(T), members);
#endif
    named_each_derived(L, members, [L](int cls) { named_inherit(L, cls); });
}
// Pushes the wrapper for a pointer whose static type is bound to a named
// class. Returns false and pushes nil when it is not.
template<class C> bool named_push_object(lua_State* L, C* object) {
    using U = std::remove_cv_t<C>;
    if (!object) { lua_pushnil(L); return true; }
    bool found = false;
#if ARKY_BINDER_RTTI
    if constexpr (std::is_polymorphic_v<U>) found = push_named_dynamic(L, typeid(*object));
#endif
    if (!found) found = push_named_bound<U>(L);
    if (!found) { lua_pushnil(L); return false; }
    return named_push_at(L, const_cast<U*>(object), lua_gettop(L));
}
} // namespace arky::detail

namespace arky {
// Result of a typed member: Lua takes ownership of the returned object and the
// class collector releases it.
template<class T> struct lua_owned { T* value; };

// Conversion of a value type for typed members of named classes. The primary
// template follows the Lua C API coercions that raw bindings rely on: numbers
// accept numeric strings and saturate to the target type, bool accepts nil,
// std::string accepts numbers and nil. Other types use converter<T> and the
// core conversions. A host may specialize it for any type, scalars included,
// to match the conversions of bindings it replaces: read reports failure
// through its result and push adds one value.
template<class T, class = void> struct named_value {
    // Plain classes are not values: they are objects of a bound named class.
    static constexpr bool enabled = !detail::object_type<T>;
    static bool read(lua_State* L, int index, T& out) {
        if constexpr (std::is_same_v<T, bool>) {
            const int type = lua_type(L, index);
            if (type != LUA_TBOOLEAN && type != LUA_TNIL) return false;
            out = lua_toboolean(L, index) != 0;
            return true;
        } else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
            if (!lua_isnumber(L, index)) return false;
            out = number<T>(lua_tonumber(L, index));
            return true;
        } else if constexpr (std::is_same_v<T, std::string>) {
            const int type = lua_type(L, index);
            if (type != LUA_TSTRING && type != LUA_TNUMBER && type != LUA_TNIL) return false;
            std::size_t size = 0;
            const char* text = lua_tolstring(L, index, &size);
            out.assign(text ? text : "", size);
            return true;
        } else {
            try { out = detail::read<T>(L, index); return true; }
            catch (const std::exception&) { return false; }
        }
    }
    static void push(lua_State* L, const T& value) {
        if constexpr (std::is_same_v<T, bool>) lua_pushboolean(L, value);
        else if constexpr (std::is_arithmetic_v<T>) lua_pushnumber(L, static_cast<lua_Number>(value));
        else if constexpr (std::is_enum_v<T>)
            lua_pushnumber(L, static_cast<lua_Number>(static_cast<std::underlying_type_t<T>>(value)));
        else if constexpr (std::is_same_v<T, std::string>) lua_pushlstring(L, value.data(), value.size());
        else detail::push(L, value);
    }
private:
    template<class N> static N number(lua_Number value) {
        if constexpr (std::is_enum_v<N>) return static_cast<N>(number<std::underlying_type_t<N>>(value));
        else if constexpr (std::is_floating_point_v<N>) return static_cast<N>(value);
        else {
            if (!(value == value)) return N{};
            if (value <= static_cast<lua_Number>(std::numeric_limits<N>::lowest())) return std::numeric_limits<N>::lowest();
            if (value >= static_cast<lua_Number>(std::numeric_limits<N>::max())) return std::numeric_limits<N>::max();
            return static_cast<N>(value);
        }
    }
};
} // namespace arky

namespace arky::detail {
template<class C> inline constexpr bool named_bound_type = std::is_class_v<C> && !named_value<C>::enabled;

// One parameter of a typed member: where its value is read from and how it is
// handed to the callable. Values live in the reader for the call.
template<class P, class = void> struct named_arg {
    using value_type = std::remove_cvref_t<P>;
    static_assert(!std::is_rvalue_reference_v<P>, "Lua arguments cannot bind to C++ rvalue references");
    static_assert(!std::is_lvalue_reference_v<P> || std::is_const_v<std::remove_reference_t<P>>,
                  "Mutable references require an object of a bound named class");
    static constexpr bool object = false;
    value_type value{};
    bool read(lua_State* L, int index) { return named_value<value_type>::read(L, index, value); }
    P get() { if constexpr (std::is_reference_v<P>) return value; else return std::move(value); }
};
// nil and an expired wrapper arrive as nullptr, as raw bindings pass them.
template<class C> struct named_arg<C*, std::enable_if_t<named_bound_type<std::remove_cv_t<C>>>> {
    static constexpr bool object = true;
    C* value = nullptr;
    bool live = true;
    bool read(lua_State* L, int index) {
        if (lua_isnoneornil(L, index)) return true;
        std::remove_cv_t<C>* found = nullptr;
        const auto match = named_bound<std::remove_cv_t<C>>(L, index, found);
        if (match == named_match::no) return false;
        live = match == named_match::yes;
        value = found;
        return true;
    }
    C* get() { return value; }
};
template<class C> struct named_arg<C&, std::enable_if_t<named_bound_type<std::remove_cv_t<C>>>> {
    static constexpr bool object = true;
    C* value = nullptr;
    bool live = true;
    bool read(lua_State* L, int index) {
        std::remove_cv_t<C>* found = nullptr;
        const auto match = named_bound<std::remove_cv_t<C>>(L, index, found);
        live = match != named_match::expired;
        value = found;
        return match == named_match::yes;
    }
    C& get() { return *value; }
};
template<> struct named_arg<const char*, void> {
    static constexpr bool object = false;
    const char* value = "";
    bool read(lua_State* L, int index) {
        const int type = lua_type(L, index);
        if (type == LUA_TNIL) return true;
        if (type != LUA_TSTRING && type != LUA_TNUMBER) return false;
        value = lua_tostring(L, index);
        return true;
    }
    const char* get() { return value; }
};

template<class> struct named_owned_type : std::false_type {};
template<class C> struct named_owned_type<lua_owned<C>> : std::true_type {};
// R is the declared result type, so a returned reference is exposed in place.
template<class R> void named_push_result(lua_State* L, std::type_identity_t<R> value) {
    using U = std::remove_cvref_t<R>;
    if constexpr (named_owned_type<U>::value) {
        if (!named_push_object(L, value.value)) throw error("C++ type is not bound to a named class");
        named_take_ownership(L, -1);
    } else if constexpr (std::is_pointer_v<U> && named_bound_type<std::remove_cv_t<std::remove_pointer_t<U>>>) {
        if (!named_push_object(L, value)) throw error("C++ type is not bound to a named class");
    } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        lua_pushstring(L, value);
    } else if constexpr (named_bound_type<U>) {
        static_assert(std::is_lvalue_reference_v<R>, "Return an object of a bound named class by pointer or reference");
        if (!named_push_object(L, &value)) throw error("C++ type is not bound to a named class");
    } else {
        named_value<U>::push(L, value);
    }
}

struct named_call { int results = 0; bool expired = false; };
// Converts the arguments of one overload and calls it. A false result means
// the arguments do not fit and the next overload is tried.
template<bool Method, bool Chain, class F, std::size_t... I>
bool named_attempt(lua_State* L, F& function, named_call& call, std::index_sequence<I...>) {
    using traits = function_traits<F>;
    using arguments = typename traits::arguments;
    std::tuple<named_arg<std::tuple_element_t<I, arguments>>...> values;
    const int first = Method ? 1 : 2;
    if (!(std::get<I>(values).read(L, first + static_cast<int>(I)) && ...)) {
        if constexpr (Method && sizeof...(I) > 0) {
            // A receiver that is the right kind of object but no longer alive ends the call.
            using receiver = std::tuple_element_t<0, std::tuple<named_arg<std::tuple_element_t<I, arguments>>...>>;
            if constexpr (receiver::object) {
                if (!std::get<0>(values).live) { call.expired = true; return true; }
            }
        }
        return false;
    }
    if constexpr (Method && sizeof...(I) > 0) {
        using receiver = std::tuple_element_t<0, std::tuple<named_arg<std::tuple_element_t<I, arguments>>...>>;
        if constexpr (receiver::object) {
            if (!std::get<0>(values).live) { call.expired = true; return true; }
            if constexpr (std::is_pointer_v<std::tuple_element_t<0, arguments>>) {
                if (!std::get<0>(values).value) return false;
            }
        }
    }
    if constexpr (std::is_void_v<typename traits::result>) {
        function(std::get<I>(values).get()...);
        if (Chain) lua_settop(L, 1);
        call.results = Chain ? 1 : 0;
    } else {
        named_push_result<typename traits::result>(L, function(std::get<I>(values).get()...));
        call.results = 1;
    }
    return true;
}
inline int named_call_error(lua_State* L, bool expired, bool receiver = true) {
    if (expired) return luaL_error(L, "expired native object");
    lua_Debug info{};
    const char* name = lua_getstack(L, 0, &info) && lua_getinfo(L, "n", &info) && info.name ? info.name : "?";
    if (!receiver) return luaL_error(L, "'%s' is called on a class: Class:%s(...)", name, name);
    return luaL_error(L, "invalid arguments to '%s' (%d given)", name, lua_gettop(L) - 1);
}
// Whether the value is the public table of a named class: the receiver of Class:name(...).
inline bool named_class_table(lua_State* L, int index) {
    if (lua_type(L, index) != LUA_TTABLE || !lua_getmetatable(L, index)) return false;
    lua_pushlightuserdata(L, &named_table_key);
    lua_rawget(L, -2);
    const bool same = lua_rawequal(L, -1, index) != 0;
    lua_pop(L, 2);
    return same;
}
// Overloads are tried in registration order among those with the given
// argument count. Slot 1 is the receiver: the object, or the class for statics.
// A static called without its class (Class.name(...), object:name(...)) is
// rejected instead of taking the first argument for the receiver.
// A C++ exception becomes a Lua error; like every Lua error of this layer it
// is raised after the callable's arguments and temporaries are destroyed.
template<bool Method, bool Chain, class... F> int named_dispatch(lua_State* L, F&... functions) {
    static_assert(!Method || (... && (function_traits<F>::arity > 0)), "A typed method needs an explicit receiver parameter");
    char message[512];
    named_call call;
    bool done = false, thrown = false, receiver = true;
    try {
        reserve(L, 8);
        if constexpr (!Method) receiver = named_class_table(L, 1);
        const int count = lua_gettop(L) - 1;
        done = receiver && ((static_cast<int>(function_traits<F>::arity) - (Method ? 1 : 0) == count
            && named_attempt<Method, Chain>(L, functions, call, std::make_index_sequence<function_traits<F>::arity>{})) || ...);
    } catch (const std::exception& exception) {
        std::snprintf(message, sizeof(message), "%s", exception.what());
        thrown = true;
    } catch (...) {
        std::snprintf(message, sizeof(message), "%s", "unknown C++ exception");
        thrown = true;
    }
    if (thrown) {
        lua_pushstring(L, message);
        return lua_error(L);
    }
    if (done && !call.expired) return call.results;
    return named_call_error(L, call.expired, receiver);
}
// Captureless callables need no storage: the thunk itself is the Lua function.
template<bool Method, bool Chain, class... F> int named_thunk(lua_State* L) {
    std::tuple<F...> functions;
    return std::apply([L](F&... each) { return named_dispatch<Method, Chain, F...>(L, each...); }, functions);
}
template<bool Method, bool Chain, class... F> int named_closure(lua_State* L) {
    auto* functions = static_cast<std::tuple<F...>*>(lua_touserdata(L, lua_upvalueindex(1)));
    return std::apply([L](F&... each) { return named_dispatch<Method, Chain, F...>(L, each...); }, *functions);
}
template<bool Method, bool Chain, class... F> void push_named_function(lua_State* L, F&&... functions) {
    static_assert(sizeof...(F) > 0, "Register at least one callable");
    using pack = std::tuple<std::decay_t<F>...>;
    static_assert(std::is_trivially_destructible_v<pack>,
                  "Typed members of named classes hold no resources: keep state in the bound object");
    if constexpr ((... && (std::is_empty_v<std::decay_t<F>> && std::is_default_constructible_v<std::decay_t<F>>))) {
        lua_pushcfunction(L, (named_thunk<Method, Chain, std::decay_t<F>...>));
    } else {
        ::new (lua_newuserdata(L, sizeof(pack))) pack(std::forward<F>(functions)...);
        lua_pushcclosure(L, (named_closure<Method, Chain, std::decay_t<F>...>), 1);
    }
}
// Member function pointers become callables with an explicit receiver.
template<class T, class R, class C, class... A> auto named_member(R (C::*function)(A...)) {
    return [function](T& self, A... args) -> R { return (self.*function)(std::forward<A>(args)...); };
}
template<class T, class R, class C, class... A> auto named_member(R (C::*function)(A...) const) {
    return [function](const T& self, A... args) -> R { return (self.*function)(std::forward<A>(args)...); };
}
template<class T, class R, class C, class... A> auto named_member(R (C::*function)(A...) noexcept) {
    return [function](T& self, A... args) -> R { return (self.*function)(std::forward<A>(args)...); };
}
template<class T, class R, class C, class... A> auto named_member(R (C::*function)(A...) const noexcept) {
    return [function](const T& self, A... args) -> R { return (self.*function)(std::forward<A>(args)...); };
}
// A raw variable getter receives (receiver, name); the typed getter sees the receiver only.
template<class F> int named_getter(lua_State* L) {
    lua_settop(L, 1);
    return named_dispatch<true, false, F>(L, *static_cast<F*>(lua_touserdata(L, lua_upvalueindex(1))));
}
} // namespace arky::detail

namespace arky {
template<class T, bool Chain> class named_class_of;

// Registration handle. The member table is a plain Lua table: it is the
// metatable of every instance and of the public class table, and raw entries
// are class members. __index, __newindex and __gc belong to the binder.
// Members can be added or replaced at any time, also after derived classes
// and objects exist; the nearest class in the base chain wins.
class named_class {
public:
    // Creates the class on first use and reopens it afterwards.
    named_class(lua_State* L, std::string_view name) : L_(L), name_(name) {
        if (!L_) throw error("named_class requires a live Lua state");
        detail::stack_guard stack(L_);
        detail::reserve(L_, 12);
        detail::named_create(L_, name_);
    }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] static bool exists(lua_State* L, std::string_view name) {
        if (!L || !lua_checkstack(L, 3)) return false;
        const bool found = detail::push_named_members(L, name);
        lua_pop(L, 1);
        return found;
    }
    // Whether class type is base or derives from it.
    [[nodiscard]] static bool derives(lua_State* L, std::string_view type, std::string_view base) {
        if (!L || !lua_checkstack(L, 4)) return false;
        const int top = lua_gettop(L);
        bool result = false;
        if (detail::push_named_members(L, type)) {
            detail::named_get(L, top + 1, &detail::named_kinds_key);
            if (lua_istable(L, -1)) {
                lua_pushlstring(L, base.data(), base.size());
                lua_rawget(L, -2);
                result = lua_toboolean(L, -1) != 0;
            }
        }
        lua_settop(L, top);
        return result;
    }
    // Class name of a public class table or member table; nullptr otherwise.
    // The string lives as long as the class. Never raises.
    [[nodiscard]] static const char* name_of(lua_State* L, int index) {
        if (!L || lua_type(L, index) != LUA_TTABLE || !lua_checkstack(L, 4)) return nullptr;
        const int top = lua_gettop(L);
        index = detail::absolute_index(L, index);
        detail::named_get(L, index, &detail::named_name_key);
        if (lua_type(L, -1) != LUA_TSTRING && lua_getmetatable(L, index)) {
            const int members = lua_gettop(L);
            detail::named_get(L, members, &detail::named_table_key);
            if (lua_rawequal(L, -1, index)) detail::named_get(L, members, &detail::named_name_key);
            else lua_pushnil(L);
        }
        const char* name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        lua_settop(L, top);
        return name;
    }

#if ARKY_BINDER_RTTI
    // Class name a polymorphic C++ type is bound to, by reg_named_class<T> or
    // by the type resolver; nullptr when there is none. Pass typeid(*object)
    // for the class of an object. The string lives as long as the class.
    [[nodiscard]] static const char* of_type(lua_State* L, const std::type_info& type) {
        if (!L || !lua_checkstack(L, 6)) return nullptr;
        const int top = lua_gettop(L);
        const char* name = nullptr;
        if (detail::push_named_dynamic(L, type)) {
            detail::named_get(L, top + 1, &detail::named_name_key);
            if (lua_type(L, -1) == LUA_TSTRING) name = lua_tostring(L, -1);
        }
        lua_settop(L, top);
        return name;
    }
    // For hosts that also register classes by name only: resolver returns the
    // name of the named class a polymorphic C++ type belongs to, or nullptr.
    // It is asked for types not bound with reg_named_class<T> whenever an
    // object is exposed through a pointer to a bound base, so the object gets
    // its own class instead of the base. It must not throw or raise.
    static void reg_type_resolver(lua_State* L, const char* (*resolver)(lua_State*, const std::type_info&) noexcept) {
        if (!L) throw error("named_class requires a live Lua state");
        detail::stack_guard stack(L);
        detail::reserve(L, 8);
        detail::push_named_dynamic_types(L).resolver = resolver;
    }
#endif

    // One base supplies inherited members and is-a checks. An unknown base is
    // created empty and can be filled in later. Register the base before
    // exposing objects: the hierarchy shares one identity cache.
    named_class& reg_base(std::string_view base) {
        detail::stack_guard stack(L_);
        detail::reserve(L_, 20);
        const int derived = push_self();
        detail::named_create(L_, base);
        const int parent = lua_gettop(L_);
        if (detail::named_derives(L_, parent, derived)) throw error("native class cannot derive from itself");
        if (lua_getmetatable(L_, derived)) {
            const bool same = lua_rawequal(L_, -1, parent) != 0;
            lua_pop(L_, 1);
            if (same) return *this;
        }
        lua_pushvalue(L_, parent);
        lua_setmetatable(L_, derived);
        lua_pushboolean(L_, true);
        detail::named_set(L_, parent, &detail::named_derived_key);
        detail::named_each_derived(L_, derived, [L = L_](int cls) { detail::named_inherit(L, cls); });
        return *this;
    }
    // Raw Lua C callbacks own argument validation and exception boundaries.
    // The same function serves obj:name(...), also for derived objects, and
    // Class.name(...) on the class that registers it.
    named_class& reg_raw_method(const char* name, lua_CFunction function) {
        if (!function) throw error("cannot register a null Lua C function");
        put(nullptr, name, [&] { lua_pushcfunction(L_, function); });
        return *this;
    }
    // Typed callables, converted with named_value. Each one names its
    // receiver as the first parameter: a pointer or reference to a C++ type
    // bound to a named class, or a host value type. Several callables form an
    // overload set: the first one that fits the argument count and types runs.
    template<class... F> named_class& reg_method(const char* name, F&&... overloads) {
        put(nullptr, name, [&] { detail::push_named_function<true, false>(L_, std::forward<F>(overloads)...); });
        return *this;
    }
    // Typed callables without a receiver, called as Class:name(...): slot 1
    // is the class table. Any other receiver is an error.
    template<class... F> named_class& reg_static(const char* name, F&&... overloads) {
        put(nullptr, name, [&] { detail::push_named_function<false, false>(L_, std::forward<F>(overloads)...); });
        return *this;
    }
    template<class V> named_class& reg_value(const char* name, V&& value) {
        put(nullptr, name, [&] { detail::push(L_, std::forward<V>(value)); });
        return *this;
    }
    // getter(receiver, name) returns one value; setter(receiver, value).
    // The receiver is the object, or the class table for Class.name.
    // Without a setter an assignment becomes an instance field.
    named_class& reg_raw_var(const char* name, lua_CFunction getter, lua_CFunction setter = nullptr) {
        if (!getter) throw error("cannot register a null getter");
        put(&detail::named_getters_key, name, [&] { lua_pushcfunction(L_, getter); });
        put(&detail::named_setters_key, name, [&] {
            if (setter) lua_pushcfunction(L_, setter); else lua_pushnil(L_);
        });
        return *this;
    }
    // Number keys on an object: getter(object, index) returns one value and
    // setter(object, index, value) stores one. Derived classes inherit both.
    named_class& reg_raw_index(lua_CFunction getter) {
        if (!getter) throw error("cannot register a null index getter");
        hook(&detail::named_index_get_key, getter);
        return *this;
    }
    named_class& reg_raw_newindex(lua_CFunction setter) {
        if (!setter) throw error("cannot register a null index setter");
        hook(&detail::named_index_set_key, setter);
        return *this;
    }
    // event is a Lua metamethod name such as "__eq" or "__add"; derived
    // classes inherit the handler. Comparisons apply between objects of the
    // same class. "__call" also receives the class table for Class(...).
    named_class& reg_raw_operator(std::string_view event, lua_CFunction function) {
        if (!function) throw error("cannot register a null operator");
        const char* field = nullptr;
        for (const char* candidate : detail::named_operator_events)
            if (event == candidate) field = candidate;
        if (!field) throw error("unsupported native operator");
        detail::stack_guard stack(L_);
        detail::reserve(L_, 20);
        const int members = push_self();
        detail::named_get(L_, members, &detail::named_operators_key);
        if (!lua_istable(L_, -1)) {
            lua_pop(L_, 1);
            lua_newtable(L_);
            lua_pushvalue(L_, -1);
            detail::named_set(L_, members, &detail::named_operators_key);
        }
        lua_pushstring(L_, field);
        lua_pushcfunction(L_, function);
        lua_rawset(L_, -3);
        lua_pushlightuserdata(L_, &detail::named_registry_key);
        lua_rawget(L_, LUA_REGISTRYINDEX);
        lua_pushlightuserdata(L_, &detail::named_operators_key);
        lua_pushboolean(L_, true);
        lua_rawset(L_, -3);
        detail::named_each_derived(L_, members, [L = L_](int cls) { detail::named_refresh_operators(L, cls); });
        return *this;
    }
    // collector(object) runs once when a Lua-owned wrapper is collected or the
    // VM closes while the object is alive. Derived classes inherit it.
    named_class& reg_collector(lua_CFunction collector) {
        if (!collector) throw error("cannot register a null collector");
        hook(&detail::named_collector_key, collector);
        return *this;
    }
    // For raw callbacks that do not check for an expired receiver. Once an
    // object is invalidated, a native function looked up through it becomes
    // one that raises "expired native object", and raw variables and number
    // keys raise the same error. Instance fields, values and script-defined
    // members stay readable. Derived classes use the nearest setting.
    named_class& enable_expired_guard(bool enabled = true) {
        detail::stack_guard stack(L_);
        detail::reserve(L_, 6);
        const int members = push_self();
        lua_pushboolean(L_, enabled);
        detail::named_set(L_, members, &detail::named_guard_key);
        return *this;
    }
    // Wrappers of this class and its descendants stand for natively owned
    // objects: exposed(pointer) runs when a wrapper is created, and with pin
    // the wrapper and its fields stay alive until invalidate or unpin. The
    // nearest class that sets this decides both.
    named_class& reg_native_lifetime(bool pin, void (*exposed)(void* object) noexcept = nullptr) {
        detail::stack_guard stack(L_);
        detail::reserve(L_, 6);
        const int members = push_self();
        ::new (lua_newuserdata(L_, sizeof(detail::named_lifetime))) detail::named_lifetime{pin, exposed};
        detail::named_set(L_, members, &detail::named_lifetime_key);
        return *this;
    }
    // Stores the public class table at a dotted global path.
    named_class& publish(std::string_view path) {
        detail::stack_guard stack(L_);
        detail::reserve(L_, 12);
        detail::validate_path(path);
        push_table();
        detail::set_path(L_, path);
        return *this;
    }

    // The following push one value on the registration state.
    void push_members() const { push_self(); }
    // The public class table reads the class's own members; inherited ones
    // are reached through objects or the class that defines them. A script
    // assignment adds or replaces a member for its objects and derived classes.
    void push_table() const {
        detail::reserve(L_, 6);
        const int members = push_self();
        detail::named_get(L_, members, &detail::named_table_key);
        if (!lua_istable(L_, -1)) {
            lua_pop(L_, 1);
            lua_newtable(L_);
            lua_pushvalue(L_, members);
            lua_setmetatable(L_, -2);
            lua_pushvalue(L_, -1);
            detail::named_set(L_, members, &detail::named_table_key);
        }
        lua_remove(L_, members);
    }
    // The value a script first replaced under name through a class table, or nil.
    void push_native(const char* name) const {
        if (!name) throw error("expected a member name");
        detail::reserve(L_, 8);
        const int top = lua_gettop(L_);
        push_self();
        while (true) {
            const int current = lua_gettop(L_);
            detail::named_get(L_, current, &detail::named_natives_key);
            if (lua_istable(L_, -1)) {
                lua_pushstring(L_, name);
                lua_rawget(L_, -2);
                lua_remove(L_, -2);
            }
            if (!lua_isnil(L_, -1)) break;
            lua_pop(L_, 1);
            if (!lua_getmetatable(L_, current)) { lua_pushnil(L_); break; }
            lua_remove(L_, current);
        }
        lua_replace(L_, top + 1);
        lua_settop(L_, top + 1);
    }
private:
    template<class, bool> friend class named_class_of;
    int push_self() const {
        detail::reserve(L_, 4);
        if (!detail::push_named_members(L_, name_)) {
            lua_pop(L_, 1);
            throw error("native class is not registered in this Lua state");
        }
        return lua_gettop(L_);
    }
    void hook(char* key, lua_CFunction function) {
        detail::stack_guard stack(L_);
        detail::reserve(L_, 6);
        const int members = push_self();
        lua_pushcfunction(L_, function);
        detail::named_set(L_, members, key);
    }
    template<class P> void put(char* table, const char* name, P&& push_value) {
        if (!name || !*name) throw error("expected a non-empty member name");
        detail::stack_guard stack(L_);
        detail::reserve(L_, 12);
        const int members = push_self();
        int target = members;
        if (table) {
            detail::named_get(L_, members, table);
            if (!lua_istable(L_, -1)) {
                lua_pop(L_, 1);
                lua_newtable(L_);
                lua_pushvalue(L_, -1);
                detail::named_set(L_, members, table);
            }
            target = lua_gettop(L_);
        }
        push_value();
        detail::named_set(L_, target, name);
    }
    lua_State* L_;
    std::string name_;
};

// Object access for raw callbacks. These functions follow the Lua C API
// convention: they take the calling state, throw no C++ exceptions and report
// failure through their result. Like other Lua API calls, the ones that
// allocate can raise a memory error.
struct named_object {
    named_object() = delete;

    // Pushes the wrapper for pointer; nullptr becomes nil. The same pointer
    // yields the same wrapper within one class hierarchy, and a wrapper first
    // exposed as a base becomes the more specific class. Returns false and
    // pushes nil when type is not registered.
    static bool push(lua_State* L, void* pointer, std::string_view type) {
        luaL_checkstack(L, 10, "native object");
        if (!pointer) { lua_pushnil(L); return true; }
        const int members = lua_gettop(L) + 1;
        if (!detail::push_named_members(L, type)) { lua_settop(L, members - 1); lua_pushnil(L); return false; }
        return detail::named_push_at(L, pointer, members);
    }
    // The same for a pointer whose C++ type is bound to a named class
    // (reg_named_class<T>). A polymorphic object is exposed as the registered
    // class of its dynamic type.
    template<class C> requires std::is_class_v<C>
    static bool push(lua_State* L, C* object) {
        luaL_checkstack(L, 10, "native object");
        return detail::named_push_object(L, object);
    }
    // Native pointer of a live named object; nullptr for anything else,
    // including an invalidated wrapper.
    [[nodiscard]] static void* pointer(lua_State* L, int index) {
        const auto* box = detail::named_box(L, index);
        return box ? box->value : nullptr;
    }
    // Whether the value is a named object of type or of a class derived from
    // it. Type identity outlives the native object: an invalidated wrapper
    // keeps its class.
    [[nodiscard]] static bool is_a(lua_State* L, int index, std::string_view type) {
        if (lua_type(L, index) != LUA_TUSERDATA || lua_objlen(L, index) != sizeof(detail::named_storage)
            || !lua_checkstack(L, 3) || !lua_getmetatable(L, index)) return false;
        lua_pushlightuserdata(L, &detail::named_kinds_key);
        lua_rawget(L, -2);
        bool result = false;
        if (lua_istable(L, -1)) {
            lua_pushlstring(L, type.data(), type.size());
            lua_rawget(L, -2);
            result = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
        return result;
    }
    // Class name of a named object, valid as long as the class; else nullptr.
    [[nodiscard]] static const char* type_name(lua_State* L, int index) {
        if (lua_type(L, index) != LUA_TUSERDATA || lua_objlen(L, index) != sizeof(detail::named_storage)
            || !lua_checkstack(L, 2) || !lua_getmetatable(L, index)) return nullptr;
        lua_pushlightuserdata(L, &detail::named_name_key);
        lua_rawget(L, -2);
        const char* name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        lua_pop(L, 2);
        return name;
    }

    // Lua ownership: the class collector runs when the wrapper is collected.
    // An owned wrapper is not kept by a native lifetime pin; own releases it.
    // own returns false when the wrapper is already owned or not alive.
    static bool own(lua_State* L, int index) { return detail::named_take_ownership(L, index); }
    static bool disown(lua_State* L, int index) {
        auto* box = detail::named_box(L, index);
        if (!box || !box->owned) return false;
        box->owned = false;
        return true;
    }
    // Keeps the wrapper and its fields alive until unpin or invalidate, for
    // natively owned objects that must keep their Lua state. Pins are scoped
    // to the class hierarchy, like wrapper identity.
    static bool pin(lua_State* L, int index) {
        const auto* box = detail::named_box(L, index);
        if (!box || !box->value) return false;
        luaL_checkstack(L, 6, "native object");
        const int top = lua_gettop(L);
        index = detail::absolute_index(L, index);
        lua_getmetatable(L, index);
        detail::named_get(L, top + 1, &detail::named_cache_key);
        const bool pinned = detail::push_named_pins(L, top + 2, true);
        if (pinned) {
            lua_pushlightuserdata(L, box->value);
            lua_pushvalue(L, index);
            lua_rawset(L, top + 3);
        }
        lua_settop(L, top);
        return pinned;
    }
    // type is any class of the object's hierarchy. A registered type allocates
    // nothing here or in invalidate, so a destructor can call both outside a
    // protected Lua call.
    static void unpin(lua_State* L, void* pointer, std::string_view type) {
        if (!L || !pointer || !lua_checkstack(L, 6)) return;
        const int top = lua_gettop(L);
        if (detail::push_named_members(L, type)) {
            detail::named_get(L, top + 1, &detail::named_cache_key);
            if (detail::push_named_pins(L, top + 2, false)) detail::named_forget(L, top + 3, pointer);
        }
        lua_settop(L, top);
    }
    // Native destruction notice, on the VM thread before the address is
    // reused. type is any class of the object's hierarchy. The wrapper loses
    // its pointer, fields, pin and ownership; the collector does not run.
    // Returns the number of wrappers invalidated.
    static std::size_t invalidate(lua_State* L, void* pointer, std::string_view type) {
        if (!L || !pointer || !lua_checkstack(L, 8)) return 0;
        const int top = lua_gettop(L);
        std::size_t count = 0;
        if (detail::push_named_members(L, type)) {
            const int cache = top + 2;
            detail::named_get(L, top + 1, &detail::named_cache_key);
            if (detail::push_named_pins(L, cache, false)) detail::named_forget(L, cache + 1, pointer);
            lua_settop(L, cache);
            if (lua_istable(L, cache)) {
                lua_pushlightuserdata(L, pointer);
                lua_rawget(L, cache);
                auto* box = detail::named_box(L, -1);
                if (box && box->value == pointer) {
                    *box = detail::named_storage{};
                    lua_pushvalue(L, LUA_REGISTRYINDEX);
                    lua_setfenv(L, -2);
                    ++count;
                }
                lua_pop(L, 1);
                detail::named_forget(L, cache, pointer);
            }
        }
        lua_settop(L, top);
        return count;
    }

    // Instance fields are one replaceable table per object. push_fields pushes
    // it, or nil when the object has none or the value is not a named object.
    static void push_fields(lua_State* L, int index) {
        luaL_checkstack(L, 2, "native object");
        if (!detail::named_box(L, index)) { lua_pushnil(L); return; }
        lua_getfenv(L, index);
        if (lua_rawequal(L, -1, LUA_REGISTRYINDEX)) { lua_pop(L, 1); lua_pushnil(L); }
    }
    // Pops a table or nil from the stack top and installs it as the fields of
    // the object at index. Returns false, still popping, for other values.
    static bool set_fields(lua_State* L, int index) {
        index = detail::absolute_index(L, index);
        const int kind = lua_type(L, -1);
        if (!detail::named_box(L, index) || (kind != LUA_TTABLE && kind != LUA_TNIL)) {
            lua_pop(L, 1);
            return false;
        }
        if (kind == LUA_TNIL) {
            lua_pop(L, 1);
            lua_pushvalue(L, LUA_REGISTRYINDEX);
        }
        lua_setfenv(L, index);
        return true;
    }
};

// Typed registration on a named class, in the same form as cpp_class<T>:
//   lua.reg_named_class<Sprite>("Sprite").reg_base<Node>().reg_func("frame", &Sprite::frame);
// The class keeps the object model of named classes (members can be replaced,
// instance fields shadow members, wrappers follow native lifetime), and T is
// bound to it: pointers and references to T convert in typed members and
// typed results, and named_object::push(L, T*) finds the class.
// A named class does not adjust pointers, so T and its registered bases must
// share one address (the registered base is the first, non-virtual base).
// Chain selects whether a member returning void yields its receiver.
template<class T, bool Chain = false> class named_class_of {
public:
    named_class_of(lua_State* L, std::string_view name) : class_(L, name) {
        detail::stack_guard stack(L);
        detail::named_bind<T>(L, class_.push_self());
    }
    [[nodiscard]] const std::string& name() const noexcept { return class_.name(); }
    // The untyped handle, for registration this class does not repeat.
    [[nodiscard]] named_class& named() noexcept { return class_; }
    // Members returning void return their receiver, so calls can be chained.
    [[nodiscard]] named_class_of<T, true> chain_void_methods() const { return named_class_of<T, true>(class_); }

    template<class Base> named_class_of& reg_base() {
        static_assert(std::is_base_of_v<Base, T> && !std::is_same_v<Base, T>,
                      "reg_base requires a distinct C++ base of the bound type");
        lua_State* L = class_.L_;
        std::string base;
        {
            detail::stack_guard stack(L);
            detail::reserve(L, 6);
            if (!detail::push_named_bound<Base>(L)) throw error("C++ base is not bound to a named class");
            detail::named_get(L, lua_gettop(L), &detail::named_name_key);
            std::size_t size = 0;
            const char* text = lua_tolstring(L, -1, &size);
            base.assign(text ? text : "", size);
        }
        class_.reg_base(base);
        return *this;
    }
    // Typed callables with an explicit receiver; several form an overload set.
    template<class... F> named_class_of& reg_method(const char* name, F&&... overloads) {
        class_.put(nullptr, name, [&] {
            detail::push_named_function<true, Chain>(class_.L_, std::forward<F>(overloads)...);
        });
        return *this;
    }
    // Member functions of T or of its bases.
    template<class F> requires std::is_member_function_pointer_v<F>
    named_class_of& reg_func(const char* name, F function) {
        if (!function) throw error("cannot register a null member function");
        return reg_method(name, detail::named_member<T>(function));
    }
    // Class:name(...) functions without a receiver, such as factories.
    template<class... F> named_class_of& reg_static(const char* name, F&&... overloads) {
        class_.put(nullptr, name, [&] {
            detail::push_named_function<false, Chain>(class_.L_, std::forward<F>(overloads)...);
        });
        return *this;
    }
    template<class C, class V> named_class_of& reg_var(const char* name, V C::* member) {
        static_assert(std::is_base_of_v<C, T>);
        if (!member) throw error("cannot register a null member variable");
        property(name, [member](const T& self) -> const V& { return self.*member; });
        if constexpr (!std::is_const_v<V>)
            setter(name, [member](T& self, const V& value) { self.*member = value; });
        return *this;
    }
    template<class G> named_class_of& reg_prop(const char* name, G getter) {
        static_assert(std::is_member_function_pointer_v<G> && detail::function_traits<G>::arity == 0);
        if (!getter) throw error("cannot register a null property getter");
        property(name, detail::named_member<T>(getter));
        return *this;
    }
    template<class G, class S> named_class_of& reg_prop(const char* name, G getter, S setter_function) {
        static_assert(std::is_member_function_pointer_v<S> && detail::function_traits<S>::arity == 1);
        if (!setter_function) throw error("cannot register a null property setter");
        reg_prop(name, getter);
        setter(name, detail::named_member<T>(setter_function));
        return *this;
    }
    // See named_class::reg_native_lifetime. exposed is a captureless callable
    // taking T*; it must not throw or call Lua.
    named_class_of& reg_native_lifetime(bool pin) { class_.reg_native_lifetime(pin); return *this; }
    template<class F> named_class_of& reg_native_lifetime(bool pin, F) {
        static_assert(std::is_empty_v<F> && std::is_default_constructible_v<F>,
                      "The exposure hook is a captureless callable");
        class_.reg_native_lifetime(pin, +[](void* object) noexcept { F{}(static_cast<T*>(object)); });
        return *this;
    }

    named_class_of& reg_raw_method(const char* name, lua_CFunction function) { class_.reg_raw_method(name, function); return *this; }
    named_class_of& reg_raw_var(const char* name, lua_CFunction getter, lua_CFunction setter_function = nullptr) {
        class_.reg_raw_var(name, getter, setter_function);
        return *this;
    }
    named_class_of& reg_raw_index(lua_CFunction getter) { class_.reg_raw_index(getter); return *this; }
    named_class_of& reg_raw_newindex(lua_CFunction setter_function) { class_.reg_raw_newindex(setter_function); return *this; }
    named_class_of& reg_raw_operator(std::string_view event, lua_CFunction function) { class_.reg_raw_operator(event, function); return *this; }
    named_class_of& reg_collector(lua_CFunction collector) { class_.reg_collector(collector); return *this; }
    named_class_of& enable_expired_guard(bool enabled = true) { class_.enable_expired_guard(enabled); return *this; }
    template<class V> named_class_of& reg_value(const char* name, V&& value) { class_.reg_value(name, std::forward<V>(value)); return *this; }
    named_class_of& publish(std::string_view path) { class_.publish(path); return *this; }
private:
    template<class, bool> friend class named_class_of;
    explicit named_class_of(named_class existing) : class_(std::move(existing)) {}
    template<class F> void property(const char* name, F function) {
        static_assert(std::is_trivially_destructible_v<F>);
        class_.put(&detail::named_getters_key, name, [&] {
            ::new (lua_newuserdata(class_.L_, sizeof(F))) F(std::move(function));
            lua_pushcclosure(class_.L_, (detail::named_getter<F>), 1);
        });
        // Without a setter, assigning the name creates an instance field.
        class_.put(&detail::named_setters_key, name, [&] { lua_pushnil(class_.L_); });
    }
    template<class F> void setter(const char* name, F function) {
        class_.put(&detail::named_setters_key, name, [&] {
            detail::push_named_function<true, false>(class_.L_, std::move(function));
        });
    }
    named_class class_;
};
} // namespace arky
