#pragma once

// Runtime-named native classes for raw (generated or legacy) bindings.
// A class is identified by a host-defined name instead of a C++ type, members
// are Lua C functions, and objects are untyped native pointers.
#include "lua_cclosure.h"
#include <cstddef>
#include <cstring>

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
} // namespace arky::detail

namespace arky {
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
    // Typed callable. The receiver arrives as its first argument, usually a
    // host value type whose converter reads a named object.
    template<class F> requires (!std::is_convertible_v<F, lua_CFunction>)
    named_class& reg_method(const char* name, F&& function) {
        put(nullptr, name, [&] { detail::push_function(L_, std::forward<F>(function)); });
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
        luaL_checkstack(L, 8, "native object");
        if (!pointer) { lua_pushnil(L); return true; }
        const int members = lua_gettop(L) + 1;
        if (!detail::push_named_members(L, type)) { lua_settop(L, members - 1); lua_pushnil(L); return false; }
        detail::named_get(L, members, &detail::named_cache_key);
        const int cache = members + 1;
        if (!lua_istable(L, cache)) { lua_settop(L, members - 1); lua_pushnil(L); return false; }
        lua_pushlightuserdata(L, pointer);
        lua_rawget(L, cache);
        const auto* box = detail::named_box(L, -1);
        if (box && box->value == pointer) {
            lua_getmetatable(L, -1);
            const bool keep = detail::named_derives(L, lua_gettop(L), members);
            lua_pop(L, 1);
            if (!keep) { lua_pushvalue(L, members); lua_setmetatable(L, -2); }
        } else {
            lua_pop(L, 1);
            ::new (lua_newuserdata(L, sizeof(detail::named_storage))) detail::named_storage{pointer, false};
            lua_pushvalue(L, members);
            lua_setmetatable(L, -2);
            lua_pushvalue(L, LUA_REGISTRYINDEX); // marks "no instance fields"
            lua_setfenv(L, -2);
            lua_pushlightuserdata(L, pointer);
            lua_pushvalue(L, -2);
            lua_rawset(L, cache);
        }
        lua_replace(L, members);
        lua_settop(L, members);
        return true;
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
    // own returns false when the wrapper is already owned or not alive.
    static bool own(lua_State* L, int index) {
        auto* box = detail::named_box(L, index);
        if (!box || !box->value || box->owned) return false;
        box->owned = true;
        return true;
    }
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
} // namespace arky
