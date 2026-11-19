#include "binder/lua_binder.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <tuple>

// A host adapter for stack-driven registration code, written against the
// public API only. Bindings of this style declare types by name, open a module
// or class as the table on top of the stack, store raw functions into it, and
// pass reserved names for class behavior. The adapter keeps that protocol and
// its naming rules; the binder keeps classes, lookup, identity and lifetime.
using arky::named_class;
using arky::named_object;

namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

// "const T" names the same class in this protocol.
std::string_view plain(const char* type) {
    std::string_view name(type ? type : "");
    if (name.starts_with("const ")) name.remove_prefix(6);
    return name;
}
void raw_set(lua_State* L, int table, const char* name) {
    table = table > 0 ? table : lua_gettop(L) + table + 1;
    lua_pushstring(L, name);
    lua_insert(L, -2);
    lua_rawset(L, table);
}
// Registration API exceptions become Lua errors outside the catch scope.
template<class F> void guarded(lua_State* L, F&& body) {
    char message[256];
    try { body(); return; }
    catch (const std::exception& exception) { std::snprintf(message, sizeof(message), "%s", exception.what()); }
    luaL_error(L, "%s", message);
}

void legacy_usertype(lua_State* L, const char* type) {
    lua_getfield(L, LUA_REGISTRYINDEX, type);
    const bool known = lua_istable(L, -1);
    lua_pop(L, 1);
    if (known) return;
    // Existing code looks the member table up as registry[type].
    guarded(L, [&] { named_class(L, plain(type)).push_members(); });
    lua_setfield(L, LUA_REGISTRYINDEX, type);
}
// Expects the enclosing module on top and stores the class table as module[local].
void legacy_class(lua_State* L, const char* local, const char* type, const char* base, lua_CFunction collector) {
    legacy_usertype(L, type);
    guarded(L, [&] {
        named_class cls(L, plain(type));
        if (base && *base) cls.reg_base(plain(base));
        if (collector) cls.reg_collector(collector);
        cls.push_table();
    });
    lua_pushboolean(L, true);
    raw_set(L, -2, ".isclass");
    raw_set(L, -2, local);
}
void legacy_module(lua_State* L, const char* name) {
    lua_newtable(L);
    raw_set(L, -2, name);
}
// Pushes the registration target: a module table, or a class's member table.
void legacy_begin(lua_State* L, const char* name) {
    if (!name) { lua_pushvalue(L, LUA_GLOBALSINDEX); return; }
    lua_pushstring(L, name);
    lua_rawget(L, -2);
    if (const char* type = named_class::name_of(L, -1)) {
        guarded(L, [&] { named_class(L, type).push_members(); });
        lua_remove(L, -2);
    }
}
void legacy_end(lua_State* L) { lua_pop(L, 1); }
void legacy_function(lua_State* L, const char* name, lua_CFunction function) {
    const char* type = name[0] == '.' ? named_class::name_of(L, -1) : nullptr;
    if (type) {
        // Reserved names describe class behavior instead of members.
        guarded(L, [&] {
            named_class cls(L, type);
            if (!std::strcmp(name, ".geti")) cls.reg_raw_index(function);
            else if (!std::strcmp(name, ".seti")) cls.reg_raw_newindex(function);
            else if (!std::strcmp(name, ".eq")) cls.reg_raw_operator("__eq", function);
            else if (!std::strcmp(name, ".collector")) cls.reg_collector(function);
        });
        return;
    }
    lua_pushcfunction(L, function);
    raw_set(L, -2, name); // a member table is a plain table
}
void legacy_variable(lua_State* L, const char* name, lua_CFunction getter, lua_CFunction setter) {
    const char* type = named_class::name_of(L, -1);
    guarded(L, [&] { named_class(L, type ? type : "").reg_raw_var(name, getter, setter); });
}
// An array is a per-object proxy whose metatable holds the element accessors.
// The getter builds it once and keeps it as an instance field.
int array_proxy(lua_State* L) {
    lua_pushfstring(L, ".array.%s", lua_tostring(L, 2));
    lua_gettable(L, 1);
    if (!lua_istable(L, -1)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushlightuserdata(L, named_object::pointer(L, 1));
    raw_set(L, -2, ".self");
    lua_pushvalue(L, -2);
    lua_setmetatable(L, -2);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, -2);
    lua_settable(L, 1);
    return 1;
}
void legacy_array(lua_State* L, const char* name, lua_CFunction getter) {
    const char* type = named_class::name_of(L, -1);
    lua_pushfstring(L, ".array.%s", name);
    lua_newtable(L);
    lua_pushcfunction(L, getter);
    raw_set(L, -2, "__index");
    lua_rawset(L, -3);
    guarded(L, [&] { named_class(L, type ? type : "").reg_raw_var(name, array_proxy); });
}
void legacy_push(lua_State* L, void* pointer, const char* type) {
    if (!named_object::push(L, pointer, plain(type))) luaL_error(L, "unregistered native type: %s", type);
}
// A script table can stand in for the object stored in its ".c_instance".
void* legacy_pointer(lua_State* L, int index) {
    if (lua_istable(L, index)) {
        lua_getfield(L, index, ".c_instance");
        if (named_object::type_name(L, -1)) lua_replace(L, index); else lua_pop(L, 1);
    }
    return named_object::pointer(L, index);
}
bool legacy_is(lua_State* L, int index, const char* type) {
    return lua_isnil(L, index) || named_object::is_a(L, index, plain(type));
}
void legacy_invalidate(lua_State* L, void* pointer, const char* type) {
    named_object::invalidate(L, pointer, plain(type));
}

// The script-side helper table of the protocol.
int lib_type(lua_State* L) {
    if (const char* name = named_object::type_name(L, 1)) { lua_pushstring(L, name); return 1; }
    const char* type = lua_istable(L, 1) ? named_class::name_of(L, 1) : nullptr;
    if (type) { lua_pushliteral(L, ".isclass"); lua_rawget(L, 1); }
    if (type && lua_isnil(L, -1)) lua_pushfstring(L, "class %s", type);
    else lua_pushstring(L, luaL_typename(L, 1));
    return 1;
}
int lib_isnull(lua_State* L) { lua_pushboolean(L, !named_object::pointer(L, 1)); return 1; }
int lib_iskindof(lua_State* L) {
    if (!lua_getmetatable(L, 1)) return luaL_error(L, "class or object expected");
    lua_pushboolean(L, legacy_is(L, 1, luaL_checkstring(L, 2)));
    return 1;
}
int lib_cast(lua_State* L) {
    void* pointer = legacy_pointer(L, 1);
    const char* type = lua_tostring(L, 2);
    if (!pointer || !type || !named_object::push(L, pointer, plain(type))) lua_pushnil(L);
    return 1;
}
int lib_setpeer(lua_State* L) {
    if (!named_object::type_name(L, 1)) return luaL_error(L, "userdata expected");
    lua_settop(L, 2);
    if (!named_object::set_fields(L, 1)) return luaL_error(L, "table or nil expected");
    return 0;
}
int lib_getpeer(lua_State* L) { named_object::push_fields(L, 1); return 1; }
int lib_inherit(lua_State* L) { lua_pushvalue(L, 2); raw_set(L, 1, ".c_instance"); return 0; }
int lib_getcfunction(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    const char* type = named_object::type_name(L, 1);
    if (!type) type = named_class::name_of(L, 1);
    if (!type) return luaL_error(L, "class or object expected");
    guarded(L, [&] { named_class(L, type).push_native(key); });
    return 1;
}
int lib_takeownership(lua_State* L) { lua_pushboolean(L, named_object::own(L, 1)); return 1; }
int lib_releaseownership(lua_State* L) { lua_pushboolean(L, named_object::disown(L, 1)); return 1; }
void legacy_open(lua_State* L) {
    const luaL_Reg functions[] = {{"type", lib_type}, {"isnull", lib_isnull}, {"iskindof", lib_iskindof},
        {"cast", lib_cast}, {"setpeer", lib_setpeer}, {"getpeer", lib_getpeer}, {"inherit", lib_inherit},
        {"getcfunction", lib_getcfunction}, {"takeownership", lib_takeownership},
        {"releaseownership", lib_releaseownership}, {nullptr, nullptr}};
    lua_newtable(L);
    luaL_register(L, nullptr, functions);
    lua_setglobal(L, "legacy");
}

// Bindings written for the protocol.
struct Object { int value = 7; } object, equal_object, base_object;
int collected = 0;
int method(lua_State* L) {
    auto* value = static_cast<Object*>(legacy_pointer(L, 1));
    if (!value) return luaL_error(L, "expired object");
    lua_pushinteger(L, value->value);
    return 1;
}
int setter(lua_State* L) { static_cast<Object*>(legacy_pointer(L, 1))->value = static_cast<int>(luaL_checkinteger(L, 2)); return 0; }
int getter(lua_State* L) {
    if (lua_gettop(L) != 2 || !lua_isstring(L, 2)) return luaL_error(L, "getter expects object and name");
    return method(L);
}
int array_get(lua_State* L) {
    lua_getfield(L, 1, ".self");
    lua_pushinteger(L, static_cast<Object*>(lua_touserdata(L, -1))->value + static_cast<int>(luaL_checkinteger(L, 2)));
    return 1;
}
int numeric_get(lua_State* L) {
    lua_pushinteger(L, static_cast<Object*>(legacy_pointer(L, 1))->value + static_cast<int>(luaL_checkinteger(L, 2)));
    return 1;
}
int collector(lua_State*) { ++collected; return 0; }
int equal(lua_State* L) {
    const auto* a = static_cast<Object*>(legacy_pointer(L, 1));
    const auto* b = static_cast<Object*>(legacy_pointer(L, 2));
    lua_pushboolean(L, a && b && a->value == b->value);
    return 1;
}
int create(lua_State* L) {
    const char* type = named_class::name_of(L, 1);
    if (!type || plain("const cc.Base") != type) return luaL_error(L, "expected the cc.Base class");
    legacy_push(L, &base_object, "cc.Base");
    return 1;
}
void install(lua_State* L) {
    legacy_open(L);
    for (const char* type : {"cc.Base", "cc.Derived", "cc.Orphan"}) legacy_usertype(L, type);
    legacy_begin(L, nullptr);
    legacy_module(L, "cc");
    legacy_begin(L, "cc");
    legacy_class(L, "Base", "cc.Base", "", collector);
    legacy_begin(L, "Base");
    legacy_function(L, "method", method);
    legacy_function(L, "create", create);
    legacy_function(L, ".eq", equal);
    legacy_function(L, ".geti", numeric_get);
    legacy_variable(L, "value", getter, setter);
    legacy_array(L, "values", array_get);
    lua_pushnumber(L, 42);
    raw_set(L, -2, "CONSTANT");
    legacy_end(L);
    legacy_class(L, "Derived", "cc.Derived", "cc.Base", collector);
    legacy_class(L, "Orphan", "cc.Orphan", "cc.UnregisteredBase", nullptr);
    legacy_end(L);
    legacy_end(L);
    // Code that bypasses the helpers reaches the member table through the registry.
    lua_getfield(L, LUA_REGISTRYINDEX, "cc.Derived");
    lua_pushcfunction(L, method);
    lua_setfield(L, -2, "late");
    lua_pop(L, 1);
    for (auto [name, pointer, type] : {std::tuple{"first", &object, "cc.Base"}, std::tuple{"obj", &object, "const cc.Derived"},
             std::tuple{"equal_object", &equal_object, "cc.Derived"}, std::tuple{"base_object", &base_object, "cc.Base"}}) {
        legacy_push(L, pointer, type);
        lua_setglobal(L, name);
    }
}
void script(lua_State* L, const char* source) {
    if (luaL_dostring(L, source)) throw std::runtime_error(lua_tostring(L, -1));
    check(lua_gettop(L) == 0, "the adapter keeps the stack balanced");
}
} // namespace

int main() {
    try {
        for (int restart = 0; restart != 3; ++restart) {
            object.value = 7;
            arky::state vm;
            auto* L = vm.get();
            install(L);
            check(lua_gettop(L) == 0, "registration keeps the stack balanced");
            script(L, R"(
                assert(not legacy.takeownership(nil) and not legacy.releaseownership(nil))
                assert(legacy.cast() == nil and legacy.cast(obj) == nil and legacy.cast(obj, 'missing.Type') == nil)
                assert(not pcall(legacy.iskindof, nil, 'cc.Base'))
                -- One wrapper per object; a base-first exposure becomes the derived class.
                assert(rawequal(first, obj) and legacy.type(obj) == 'cc.Derived')
                assert(not rawequal(obj, equal_object) and obj == equal_object and obj ~= base_object)
                assert(legacy.type(cc.Derived) == 'table' and cc.Derived['.isclass'] == true)
                assert(legacy.type(getmetatable(cc.Derived)) == 'class cc.Derived')
                assert(legacy.iskindof(obj, 'cc.Base') and legacy.iskindof(obj, 'const cc.Base'))
                assert(legacy.iskindof(obj, 'cc.UnregisteredBase') == false)
                assert(rawequal(legacy.cast(obj, 'cc.Base'), obj) and legacy.type(obj) == 'cc.Derived')
                -- Objects inherit members; a class table exposes its own class.
                assert(cc.Base.method(obj) == 7 and obj:method() == 7 and obj:late() == 7)
                assert(cc.Derived.method == nil and cc.Derived.late(obj) == 7)
                assert(rawequal(cc.Base:create(), base_object) and not pcall(cc.Base.create, cc.Derived))
                assert(obj.CONSTANT == 42 and cc.Base.CONSTANT == 42)
                assert(obj.missing == nil and legacy.getpeer(obj) == nil)
                obj.value = 31; assert(obj.value == 31)
                assert(obj[2] == 33 and obj.values[4] == 35 and rawequal(obj.values, obj.values))
                -- Instance fields and per-object overrides of native methods.
                obj.foo = {nested = 9}; assert(obj.foo.nested == 9)
                local original = obj.method
                obj.method = function(self) return original(self) + 10 end
                assert(obj:method() == 41)
                obj.method = false; assert(obj.method == false)
                obj.method = original; assert(obj:method() == 31)
                legacy.setpeer(obj, setmetatable({tag = 5}, {__index = {from_class = 8}}))
                assert(obj.tag == 5 and obj.from_class == 8)
                obj.extra = 9; assert(obj.extra == 9 and obj.tag == 5)
                legacy.setpeer(obj, nil); assert(legacy.getpeer(obj) == nil and obj:method() == 31)
                -- A script table stands in for the native object it inherits from.
                do
                    local stand_in = {}; legacy.inherit(stand_in, obj)
                    assert(cc.Base.method(stand_in) == 31)
                end
                -- Class-level override through the class table, with the native original kept.
                local base_method = cc.Base.method
                cc.Base.method = function() return 77 end
                assert(obj:method() == 77 and legacy.getcfunction(cc.Base, 'method') == base_method)
                assert(legacy.getcfunction(obj, 'method') == base_method)
                cc.Base.method = base_method
                local co = coroutine.create(function() assert(obj:method() == 31) end)
                assert(coroutine.resume(co))
                assert(legacy.takeownership(obj) and legacy.releaseownership(obj) and legacy.takeownership(obj))
                local cycle = obj
                obj.callback = function() return cycle end
                obj = nil; first = nil; collectgarbage(); collectgarbage()
                cycle = nil; collectgarbage(); collectgarbage()
            )");
            check(collected == restart + 1, "a Lua-owned wrapper in a field cycle is collected once");
            // The same address must not bring back the previous wrapper or its fields.
            legacy_push(L, &object, "cc.Derived");
            lua_pushliteral(L, "old");
            lua_setfield(L, -2, "note");
            lua_setglobal(L, "expired");
            legacy_invalidate(L, &object, "cc.Base");
            legacy_push(L, &object, "cc.Derived");
            lua_setglobal(L, "fresh");
            script(L, R"(
                assert(legacy.isnull(expired) and not legacy.isnull(fresh) and not rawequal(expired, fresh))
                assert(expired.note == nil and fresh.note == nil and legacy.type(expired) == 'cc.Derived')
                assert(type(expired.method) == 'function' and not pcall(function() expired:method() end))
                assert(fresh:method() == 31)
            )");
        }
        check(collected == 3, "closing a VM does not collect objects Lua does not own");
        std::cout << "named_adapter: " << checks << " checks passed\n";
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
