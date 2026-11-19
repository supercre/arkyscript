#include "binder/lua_binder.h"
#include <iostream>

using namespace arky;

namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class F> void fails(F&& action, std::string_view expected) {
    try { action(); }
    catch (const error& exception) {
        check(std::string_view(exception.what()).find(expected) != std::string_view::npos, exception.what());
        return;
    }
    throw std::runtime_error("expected a named class error");
}

struct Node {
    static inline int live = 0, collected = 0;
    int x = 0;
    Node() { ++live; }
    virtual ~Node() noexcept { --live; }
};
struct Sprite : Node { int frame = 5; };
struct Inner { int value = 3; };
struct Outer { Inner inner; int value = 9; };

// Raw callbacks validate their own arguments and raise Lua errors.
Node* node_arg(lua_State* L, int index) {
    auto* node = named_object::is_a(L, index, "Node")
        ? static_cast<Node*>(named_object::pointer(L, index)) : nullptr;
    if (!node) luaL_error(L, "expected a live Node at argument %d", index);
    return node;
}
int node_move(lua_State* L) {
    node_arg(L, 1)->x += static_cast<int>(luaL_checkinteger(L, 2));
    lua_settop(L, 1);
    return 1;
}
int node_x(lua_State* L) { lua_pushinteger(L, node_arg(L, 1)->x); return 1; }
int node_twice(lua_State* L) { lua_pushinteger(L, node_arg(L, 1)->x * 2); return 1; }
int node_get_pos(lua_State* L) {
    if (lua_gettop(L) != 2 || lua_type(L, 2) != LUA_TSTRING) return luaL_error(L, "getter expects receiver and name");
    if (lua_istable(L, 1)) { lua_pushliteral(L, "class access"); return 1; }
    lua_pushinteger(L, node_arg(L, 1)->x);
    return 1;
}
int node_set_pos(lua_State* L) {
    if (lua_gettop(L) != 2) return luaL_error(L, "setter expects receiver and value");
    node_arg(L, 1)->x = static_cast<int>(luaL_checkinteger(L, 2));
    return 0;
}
int node_get_readonly(lua_State* L) { lua_pushinteger(L, node_arg(L, 1)->x + 100); return 1; }
int node_index(lua_State* L) {
    lua_pushinteger(L, node_arg(L, 1)->x + static_cast<int>(lua_tointeger(L, 2)));
    return 1;
}
int node_newindex(lua_State* L) {
    if (lua_gettop(L) != 3) return luaL_error(L, "index setter expects object, index and value");
    node_arg(L, 1)->x = static_cast<int>(lua_tointeger(L, 2) * lua_tointeger(L, 3));
    return 0;
}
int node_collect(lua_State* L) {
    delete static_cast<Node*>(named_object::pointer(L, 1));
    ++Node::collected;
    return 0;
}
int node_create(lua_State* L) {
    named_object::push(L, new Node, "Node");
    named_object::own(L, -1);
    return 1;
}
int sprite_create(lua_State* L) {
    named_object::push(L, new Sprite, "Sprite");
    named_object::own(L, -1);
    return 1;
}
int sprite_frame(lua_State* L) {
    auto* sprite = named_object::is_a(L, 1, "Sprite") ? static_cast<Sprite*>(named_object::pointer(L, 1)) : nullptr;
    if (!sprite) return luaL_error(L, "expected a live Sprite");
    lua_pushinteger(L, sprite->frame);
    return 1;
}
int node_equal(lua_State* L) {
    lua_pushboolean(L, node_arg(L, 1)->x == node_arg(L, 2)->x);
    return 1;
}
int node_add(lua_State* L) { lua_pushinteger(L, node_arg(L, 1)->x + node_arg(L, 2)->x); return 1; }
int node_call(lua_State* L) {
    lua_pushinteger(L, lua_gettop(L));
    lua_pushboolean(L, lua_istable(L, 1));
    return 2;
}
int cache_builds = 0;
// A getter can keep its result as an instance field; later reads skip the getter.
int node_cached(lua_State* L) {
    ++cache_builds;
    lua_newtable(L);
    lua_pushinteger(L, node_arg(L, 1)->x);
    lua_setfield(L, -2, "x");
    lua_pushvalue(L, 2);
    lua_pushvalue(L, -2);
    lua_settable(L, 1);
    return 1;
}

void set_global(lua_State* L, const char* name, void* pointer, const char* type) {
    check(named_object::push(L, pointer, type), "push a registered named class");
    lua_setglobal(L, name);
}
void register_nodes(binder& lua) {
    lua.reg_named_class("Node")
        .reg_raw_method("create", node_create)
        .reg_raw_method("move", node_move)
        .reg_raw_method("x", node_x)
        .reg_value("KIND", 7)
        .reg_raw_var("pos", node_get_pos, node_set_pos)
        .reg_raw_var("readonly", node_get_readonly)
        .reg_collector(node_collect)
        .publish("engine.Node");
    lua.reg_named_class("Sprite")
        .reg_base("Node")
        .reg_raw_method("create", sprite_create)
        .reg_raw_method("frame", sprite_frame)
        .publish("engine.Sprite");
}

void registration_and_lookup() {
    Node::collected = 0;
    {
        state vm;
        binder lua(vm);
        auto* L = vm.get();
        register_nodes(lua);
        check(named_class::exists(L, "Node") && named_class::exists(L, "Sprite"), "classes exist by name");
        check(!named_class::exists(L, "Missing") && !named_class::exists(L, ""), "unknown names do not exist");
        check(lua.reg_named_class("Sprite").name() == "Sprite", "a handle keeps its class name");
        fails([&] { lua.reg_named_class(""); }, "non-empty native class name");
        fails([&] { lua.reg_named_class("Node").reg_raw_method("bad", nullptr); }, "null Lua C function");
        fails([&] { lua.reg_named_class("Node").reg_raw_method("", node_x); }, "non-empty member name");
        fails([&] { lua.reg_named_class("Node").reg_raw_var("bad", nullptr); }, "null getter");
        fails([&] { lua.reg_named_class("Node").reg_collector(nullptr); }, "null collector");
        lua.dostring(R"(
            local sprite = engine.Sprite.create()
            assert(rawequal(sprite:move(3), sprite) and sprite:x() == 3 and sprite:frame() == 5)
            -- A class table exposes its own class; objects inherit from the base.
            assert(engine.Node.x(sprite) == 3 and engine.Sprite.frame(sprite) == 5)
            assert(engine.Sprite.move == nil and engine.Sprite.create ~= engine.Node.create)
            assert(sprite.KIND == 7 and engine.Node.KIND == 7 and engine.Sprite.KIND == nil)
            assert(engine.Sprite.pos == nil and engine.Node.frame == nil)
            sprite.pos = 11
            assert(sprite.pos == 11 and sprite:x() == 11 and engine.Node.pos == 'class access')
            assert(sprite.readonly == 111 and sprite.missing == nil and sprite[1] == nil)
            assert(not pcall(sprite.frame, engine.Node.create()))
            assert(not pcall(sprite.move, sprite, 'bad'))
            kept = sprite
        )");
        // Registration stays open after derived classes and objects exist.
        lua.reg_named_class("Node").reg_raw_method("twice", node_twice)
            .reg_raw_index(node_index).reg_raw_newindex(node_newindex);
        fails([&] { lua.reg_named_class("Node").reg_raw_index(nullptr); }, "null index getter");
        fails([&] { lua.reg_named_class("Node").reg_raw_newindex(nullptr); }, "null index setter");
        check(named_class::derives(L, "Sprite", "Node") && named_class::derives(L, "Node", "Node"), "classes know their bases");
        check(!named_class::derives(L, "Node", "Sprite") && !named_class::derives(L, "Missing", "Node")
              && !named_class::derives(L, "Node", "Missing"), "a base or unknown class is not a derived kind");
        lua.dostring(R"(
            assert(kept:twice() == 22 and kept[4] == 15)
            kept[3] = 5; assert(kept:x() == 15)
            kept['3'] = 'field'; assert(kept['3'] == 'field' and kept:x() == 15)
        )");
        lua.reg_named_class("Sprite").reg_raw_method("twice", node_x);
        lua.dostring("assert(kept:twice() == 15 and engine.Node.create():twice() == 0)");

        lua_getglobal(L, "engine");
        lua_getfield(L, -1, "Sprite");
        check(std::string_view(named_class::name_of(L, -1)) == "Sprite", "class tables report their class");
        lua.reg_named_class("Sprite").push_members();
        check(std::string_view(named_class::name_of(L, -1)) == "Sprite", "member tables report their class");
        check(lua_getmetatable(L, -2) && lua_rawequal(L, -1, -2), "a class table uses the member table as metatable");
        lua_getglobal(L, "kept");
        check(lua_getmetatable(L, -1) && lua_rawequal(L, -1, -4), "an object uses the member table as metatable");
        check(!named_class::name_of(L, -2) && !named_class::name_of(L, -6) && !named_class::name_of(L, 99),
              "objects and plain tables are not classes");
        lua_settop(L, 0);
        lua.dostring("kept = nil; collectgarbage('collect'); collectgarbage('collect')");
        check(Node::live == 0 && Node::collected == 3, "collected owners run the inherited collector once");
        lua.dostring("closing = engine.Sprite.create()");
        check(Node::live == 1, "a referenced owner stays alive");
    }
    check(Node::live == 0 && Node::collected == 4, "closing the VM collects remaining Lua-owned objects");
}

void script_extension_and_native_backup() {
    state vm;
    binder lua(vm);
    auto* L = vm.get();
    register_nodes(lua);
    Sprite sprite;
    set_global(L, "sprite", &sprite, "Sprite");
    lua.dostring(R"(
        native_move = engine.Node.move
        engine.Node.move = function(self, n) return native_move(self, n * 2) end
        assert(sprite:move(2):x() == 4)
        engine.Sprite.move = function(self, n) return native_move(self, n * 10) end
        assert(sprite:move(1):x() == 14)
        engine.Node.helper = function(self) return self:x() + 1 end
        assert(sprite:helper() == 15 and engine.Sprite.helper == nil)
        -- A member table reached through its base is a plain prototype chain.
        local members = getmetatable(sprite)
        assert(rawequal(members, getmetatable(engine.Sprite)) and rawget(members, 'helper') == nil)
        assert(members.helper == engine.Node.helper and members.pos == nil)
        members.raw = 5; assert(rawget(members, 'raw') == 5 and sprite.raw == 5 and engine.Node.raw == nil)
        members.raw = nil
        engine.Node.helper = nil
        assert(sprite.helper == nil)
    )");
    lua_getglobal(L, "native_move");
    lua.reg_named_class("Sprite").push_native("move");
    check(lua_rawequal(L, -1, -2) != 0, "a derived class finds the replaced native member");
    lua.reg_named_class("Node").push_native("move");
    check(lua_rawequal(L, -1, -3) != 0, "the first replaced value is kept");
    lua.reg_named_class("Node").push_native("x");
    lua.reg_named_class("Node").push_native("missing");
    check(lua_isnil(L, -1) && lua_isnil(L, -2), "untouched and unknown members have no backup");
    lua.reg_named_class("Sprite").push_native("helper");
    check(lua_isfunction(L, -1) && !lua_iscfunction(L, -1), "a replaced script member is kept the same way");
    lua_settop(L, 0);
    lua.dostring("engine.Sprite.move = nil; engine.Node.move = native_move; assert(sprite:move(1):x() == 15)");
    check(sprite.x == 15, "restoring the native member reaches the same object");
    // A class table reaches the setters of its own class; elsewhere it defines a member.
    fails([&] { lua.dostring("engine.Node.pos = 3"); }, "expected a live Node");
    lua.dostring("engine.Sprite.pos = 3; assert(sprite.pos == 3 and sprite:x() == 15); engine.Sprite.pos = nil; assert(sprite.pos == 15)");
    // Table-driven registration code can store raw members into the member table.
    lua.reg_named_class("Node").push_members();
    lua_pushcfunction(L, node_twice);
    lua_setfield(L, -2, "raw_twice");
    lua_pushinteger(L, 3);
    lua_setfield(L, -2, "RAW");
    lua_pop(L, 1);
    lua.dostring("assert(sprite:raw_twice() == 30 and engine.Node.raw_twice(sprite) == 30 and sprite.RAW == 3)");
    check(lua_gettop(L) == 0, "class table access keeps the stack balanced");
}

void identity_and_refinement() {
    state vm;
    binder lua(vm);
    auto* L = vm.get();
    register_nodes(lua);
    lua.reg_named_class("Outer");
    lua.reg_named_class("Inner");
    Sprite sprite;
    Node node;
    Outer outer;
    const int top = lua_gettop(L);
    check(named_object::push(L, static_cast<Node*>(&sprite), "Node"), "expose as the base class");
    check(std::string_view(named_object::type_name(L, -1)) == "Node", "the wrapper starts as the requested class");
    check(named_object::is_a(L, -1, "Node") && !named_object::is_a(L, -1, "Sprite"), "a base wrapper is not a derived kind");
    lua_pushliteral(L, "kept");
    lua_setfield(L, -2, "tag");
    check(named_object::push(L, &sprite, "Sprite"), "expose the same pointer as the derived class");
    check(lua_rawequal(L, -1, -2) != 0, "the same pointer keeps one wrapper");
    check(std::string_view(named_object::type_name(L, -2)) == "Sprite", "a derived exposure refines the wrapper");
    check(named_object::is_a(L, -1, "Node") && named_object::is_a(L, -1, "Sprite"), "a derived wrapper is both kinds");
    lua_getfield(L, -1, "tag");
    check(std::string_view(lua_tostring(L, -1)) == "kept", "refinement keeps instance fields");
    lua_pop(L, 1);
    check(named_object::push(L, static_cast<Node*>(&sprite), "Node") && lua_rawequal(L, -1, -2), "a base exposure reuses it");
    check(std::string_view(named_object::type_name(L, -1)) == "Sprite", "a base exposure never widens the wrapper");
    check(named_object::pointer(L, -1) == &sprite, "the wrapper keeps the native pointer");
    check(named_object::push(L, &node, "Node") && !lua_rawequal(L, -1, -2), "different pointers get different wrappers");
    lua_settop(L, top);

    // Unrelated hierarchies may share an address without sharing a wrapper.
    check(static_cast<void*>(&outer) == static_cast<void*>(&outer.inner), "exercise an address shared by two objects");
    check(named_object::push(L, &outer, "Outer") && named_object::push(L, &outer.inner, "Inner"), "expose both");
    check(!lua_rawequal(L, -1, -2), "each hierarchy keeps its own identity");
    check(std::string_view(named_object::type_name(L, -2)) == "Outer"
          && std::string_view(named_object::type_name(L, -1)) == "Inner", "neither wrapper is retyped");
    check(!named_object::is_a(L, -1, "Outer") && !named_object::is_a(L, -1, "Missing"), "unrelated and unknown kinds fail");
    check(!named_object::is_a(L, -1, "Sprit") && !named_object::is_a(L, -1, "Sprite2") && !named_object::is_a(L, -1, ""),
          "a kind is matched by its whole name");
    check(named_object::identified_pointer(L, -1) == named_object::pointer(L, -1) && named_object::pointer(L, -1) != nullptr,
          "a raw callback that checked the kind reads the pointer directly");
    lua_pushnumber(L, 1);
    lua_newuserdata(L, 64);
    check(!named_object::identified_pointer(L, -2) && !named_object::identified_pointer(L, -1) && !named_object::is_a(L, -1, "Node"),
          "other values have no object pointer");
    lua_pop(L, 2);
    lua_settop(L, top);

    check(!named_object::push(L, &node, "Missing") && lua_isnil(L, -1), "an unregistered class pushes nil");
    check(named_object::push(L, nullptr, "Node") && lua_isnil(L, -1), "a null pointer is nil");
    check(named_object::push(L, nullptr, "Missing") && lua_isnil(L, -1), "null needs no class");
    lua_settop(L, top);

    // The cache is weak: an unreferenced wrapper is rebuilt, a referenced one is not.
    set_global(L, "held", &node, "Node");
    lua.dostring("held.tag = 1; weak = setmetatable({held}, {__mode = 'v'}); held = nil; collectgarbage('collect'); collectgarbage('collect')");
    lua.dostring("assert(weak[1] == nil)");
    set_global(L, "held", &node, "Node");
    lua.dostring("assert(held.tag == nil and held:x() == 0)");
    check(lua_gettop(L) == top, "identity checks keep the stack balanced");
}

void instance_fields() {
    state vm;
    binder lua(vm);
    auto* L = vm.get();
    register_nodes(lua);
    Sprite a, b;
    set_global(L, "a", &a, "Sprite");
    set_global(L, "b", &b, "Sprite");
    lua.dostring(R"(
        a.tag = {name = 'first'}; b.tag = 'second'
        assert(a.tag.name == 'first' and b.tag == 'second' and tag == nil)
        local key = {}; a[key] = 'table key'; assert(a[key] == 'table key' and b[key] == nil)
        -- Fields shadow members with any value, for this object only.
        local original = a.move
        a.move = function(self, n) self.calls = (self.calls or 0) + 1; return original(self, n) end
        assert(a:move(2):x() == 2 and a.calls == 1 and b.calls == nil and b.move == original)
        a.move = false; assert(a.move == false)
        a.move = nil; assert(a.move == original)
        a.readonly = 'shadow'; assert(a.readonly == 'shadow' and b.readonly == 100)
        a.readonly = nil; assert(a.readonly == 102)
        a.KIND = 8; assert(a.KIND == 8 and b.KIND == 7 and engine.Node.KIND == 7)
    )");
    lua_getglobal(L, "a");
    named_object::push_fields(L, -1);
    check(lua_istable(L, -1), "assigned fields live in one table per object");
    lua_getfield(L, -1, "KIND");
    check(lua_tointeger(L, -1) == 8, "the field table holds the raw values");
    lua_pop(L, 2);
    // A host or script can replace the whole field table, metatable included.
    lua.dostring("peer = setmetatable({own = 1}, {__index = {inherited = 2, move = function() return 'peer' end}})");
    lua_getglobal(L, "peer");
    check(named_object::set_fields(L, -2), "install a field table");
    lua.dostring(R"(
        assert(a.own == 1 and a.inherited == 2 and a.tag == nil and a:move() == 'peer')
        a.extra = 3; assert(rawget(peer, 'extra') == 3 and a.extra == 3)
    )");
    named_object::push_fields(L, -1);
    lua_getglobal(L, "peer");
    check(lua_rawequal(L, -1, -2) != 0, "push_fields returns the installed table");
    lua_pop(L, 2);
    lua_pushnil(L);
    check(named_object::set_fields(L, -2), "nil removes the fields");
    named_object::push_fields(L, -1);
    check(lua_isnil(L, -1), "an object without fields reports nil");
    lua_pop(L, 1);
    lua.dostring("assert(a.own == nil and a:move(1):x() == 3)");
    lua_pushinteger(L, 5);
    check(!named_object::set_fields(L, -2) && lua_gettop(L) == 1, "only a table or nil is accepted and the value is popped");
    lua_newtable(L);
    lua_pushvalue(L, -1);
    check(!named_object::set_fields(L, -2) && lua_gettop(L) == 2, "only named objects carry fields");
    named_object::push_fields(L, -1);
    check(lua_isnil(L, -1), "other values have no fields");
    lua_settop(L, 0);
    lua.reg_named_class("Node").reg_raw_var("cached", node_cached);
    lua.dostring("local first = a.cached; assert(first.x == 3 and rawequal(a.cached, first) and b.cached.x == 0)");
    check(cache_builds == 2, "a getter result kept as a field is built once per object");
    // Fields are not roots: a wrapper that captures itself still collects.
    lua.dostring(R"(
        b.self = b
        b.callback = (function(owner) return function() return owner end end)(b)
        weak = setmetatable({b}, {__mode = 'v'})
        b = nil; collectgarbage('collect'); collectgarbage('collect')
        assert(weak[1] == nil)
    )");
    check(lua_gettop(L) == 0, "field access keeps the stack balanced");
}

void ownership() {
    Node::collected = 0;
    {
        state vm;
        binder lua(vm);
        auto* L = vm.get();
        register_nodes(lua);
        auto* owned = new Sprite;
        auto* released = new Sprite;
        Node borrowed;
        set_global(L, "owned", owned, "Sprite");
        set_global(L, "released", released, "Sprite");
        set_global(L, "borrowed", &borrowed, "Node");
        lua_getglobal(L, "owned");
        check(named_object::own(L, -1) && !named_object::own(L, -1), "ownership is taken once");
        lua_getglobal(L, "released");
        check(!named_object::disown(L, -1) && named_object::own(L, -1) && named_object::disown(L, -1)
              && !named_object::disown(L, -1), "ownership can be given back");
        lua_pushinteger(L, 1);
        check(!named_object::own(L, -1) && !named_object::disown(L, -1), "only named objects can be owned");
        lua_settop(L, 0);
        lua.dostring("owned = nil; released = nil; borrowed = nil; collectgarbage('collect'); collectgarbage('collect')");
        check(Node::collected == 1 && Node::live == 2, "only the owned wrapper runs its collector");
        delete released;
        // The collector sees a live object; a failing collector still expires the wrapper.
        lua.reg_named_class("Failing").reg_collector(+[](lua_State* state) -> int {
            if (!named_object::pointer(state, 1)) return luaL_error(state, "collector needs a live object");
            ++Node::collected;
            return luaL_error(state, "intentional collector failure (expected by this test)");
        });
        set_global(L, "failing", &borrowed, "Failing");
        lua_getglobal(L, "failing");
        check(named_object::own(L, -1), "own an object whose collector fails");
        lua_settop(L, 0);
        // LuaJIT reports a finalizer error on stderr or raises it, depending on its version.
        try { lua.dostring("failing = nil; collectgarbage('collect'); collectgarbage('collect')"); }
        catch (const error& exception) {
            check(std::string_view(exception.what()).find("intentional collector failure") != std::string_view::npos,
                  exception.what());
        }
        check(Node::collected == 2, "a failing collector ran exactly once");
        lua.dostring("collectgarbage('collect')");
        check(Node::collected == 2 && lua_gettop(L) == 0, "an expired wrapper is not collected twice");
        set_global(L, "closing", new Node, "Node");
        lua_getglobal(L, "closing");
        named_object::own(L, -1);
        lua_settop(L, 0);
    }
    check(Node::collected == 3 && Node::live == 0, "VM shutdown collects Lua-owned objects");
}

void pins_and_invalidation() {
    state vm;
    binder lua(vm);
    auto* L = vm.get();
    register_nodes(lua);
    auto* sprite = new Sprite;
    check(named_object::push(L, sprite, "Sprite") && named_object::pin(L, -1), "pin a natively owned object");
    lua_pushliteral(L, "state");
    lua_setfield(L, -2, "tag");
    lua_pop(L, 1);
    lua.dostring("collectgarbage('collect'); collectgarbage('collect')");
    set_global(L, "pinned", sprite, "Sprite");
    lua.dostring("assert(pinned.tag == 'state'); weak = setmetatable({pinned}, {__mode = 'v'})");
    // Destruction notice: the wrapper expires but keeps its class.
    check(lua.invalidate_named_object(static_cast<Node*>(sprite), "Node") == 1, "any class of the hierarchy finds the wrapper");
    delete sprite;
    lua_getglobal(L, "pinned");
    check(!named_object::pointer(L, -1), "an invalidated wrapper has no pointer");
    check(named_object::is_a(L, -1, "Node") && std::string_view(named_object::type_name(L, -1)) == "Sprite",
          "an invalidated wrapper keeps its class");
    check(!named_object::own(L, -1) && !named_object::pin(L, -1), "an invalidated wrapper cannot be owned or pinned");
    named_object::push_fields(L, -1);
    check(lua_isnil(L, -1), "invalidation drops instance fields");
    lua_settop(L, 0);
    lua.dostring(R"(
        assert(pinned.tag == nil and type(pinned.move) == 'function')
        assert(not pcall(pinned.move, pinned, 1) and not pcall(function() return pinned.pos end))
        pinned = nil; collectgarbage('collect'); collectgarbage('collect')
        assert(weak[1] == nil)
    )");
    check(named_object::invalidate(L, sprite, "Sprite") == 0, "a second notice finds nothing");
    check(named_object::invalidate(L, nullptr, "Sprite") == 0 && named_object::invalidate(L, &vm, "Missing") == 0,
          "null pointers and unknown classes are ignored");

    // A reused address gets a new wrapper; the stale one stays expired.
    Node first;
    set_global(L, "stale", &first, "Node");
    lua.dostring("stale.tag = 'old'");
    check(named_object::invalidate(L, &first, "Node") == 1, "invalidate an unpinned wrapper");
    set_global(L, "fresh", &first, "Node");
    lua.dostring("assert(not rawequal(stale, fresh) and fresh.tag == nil and fresh:x() == 0)");
    lua_getglobal(L, "stale");
    lua_getglobal(L, "fresh");
    check(!named_object::pointer(L, -2) && named_object::pointer(L, -1) == &first, "only the new wrapper is alive");
    lua_settop(L, 0);

    // Lua ownership ends with the notice: the collector must not run.
    Node::collected = 0;
    Node gone;
    set_global(L, "owned", &gone, "Node");
    lua_getglobal(L, "owned");
    check(named_object::own(L, -1), "own before the native side destroys the object");
    lua_settop(L, 0);
    check(named_object::invalidate(L, &gone, "Node") == 1, "native destruction wins over Lua ownership");
    lua.dostring("owned = nil; collectgarbage('collect'); collectgarbage('collect')");
    check(Node::collected == 0, "an invalidated owner does not collect");

    // unpin alone leaves the object usable and collectable.
    Node loose;
    check(named_object::push(L, &loose, "Node") && named_object::pin(L, -1), "pin again");
    lua_pop(L, 1);
    lua.dostring("collectgarbage('collect')");
    set_global(L, "loose", &loose, "Node");
    lua.dostring("loose.tag = 1; weak = setmetatable({loose}, {__mode = 'v'}); loose = nil; collectgarbage('collect')");
    lua.dostring("assert(weak[1] ~= nil and weak[1].tag == 1)");
    // Pins follow the hierarchy: other pointers, classes and hierarchies leave it alone.
    lua.reg_named_class("Outer");
    lua.reg_named_class("Inner");
    named_object::unpin(L, &vm, "Node");
    named_object::unpin(L, &loose, "Outer");
    named_object::unpin(L, &loose, "Missing");
    named_object::unpin(L, nullptr, "Node");
    lua.dostring("collectgarbage('collect'); assert(weak[1] ~= nil)");
    named_object::unpin(L, &loose, "Sprite");
    named_object::unpin(L, &loose, "Node");
    lua.dostring("collectgarbage('collect'); collectgarbage('collect'); assert(weak[1] == nil)");
    lua_pushinteger(L, 1);
    check(!named_object::pin(L, -1), "only named objects can be pinned");
    lua_settop(L, 0);
    // Two hierarchies can pin objects that share an address.
    Outer outer;
    check(named_object::push(L, &outer, "Outer") && named_object::pin(L, -1), "pin the enclosing object");
    lua_pushliteral(L, "outer");
    lua_setfield(L, -2, "tag");
    check(named_object::push(L, &outer.inner, "Inner") && named_object::pin(L, -1), "pin its first member");
    lua_pushliteral(L, "inner");
    lua_setfield(L, -2, "tag");
    lua_settop(L, 0);
    lua.dostring("collectgarbage('collect'); collectgarbage('collect')");
    check(named_object::invalidate(L, &outer.inner, "Inner") == 1, "invalidate the member alone");
    lua.dostring("collectgarbage('collect'); collectgarbage('collect')");
    check(named_object::push(L, &outer, "Outer"), "the enclosing object is still pinned");
    lua_getfield(L, -1, "tag");
    check(lua_isstring(L, -1) && std::string_view(lua_tostring(L, -1)) == "outer", "its fields survived");
    lua_settop(L, 0);
    check(named_object::invalidate(L, &outer, "Outer") == 1, "invalidate the enclosing object");
    check(lua_gettop(L) == 0, "pin and invalidation keep the stack balanced");
}

void expired_guard() {
    state vm;
    binder lua(vm);
    auto* L = vm.get();
    register_nodes(lua);
    // Raw callbacks written without an expiry check are kept out of reach.
    lua.reg_named_class("Node").enable_expired_guard().reg_raw_index(node_index).reg_raw_newindex(node_newindex);
    lua.reg_named_class("Loose").reg_base("Sprite").enable_expired_guard(false);
    Sprite guarded, live, loose;
    set_global(L, "guarded", &guarded, "Sprite");
    set_global(L, "live", &live, "Sprite");
    set_global(L, "loose", &loose, "Loose");
    lua.dostring(R"(
        guarded.tag = 1; loose.tag = 2
        engine.Node.script = function(self) return 'script' end
        native_move = guarded.move
        assert(guarded[2] == 2 and guarded.pos == 0)
    )");
    check(lua.invalidate_named_object(&guarded, "Node") == 1 && lua.invalidate_named_object(&loose, "Node") == 1,
          "expire a guarded and an unguarded object");
    lua.dostring(R"(
        local function refused(action)
            local ok, message = pcall(action)
            return not ok and tostring(message):find('expired native object', 1, true) ~= nil
        end
        -- Fields are gone; values and script-defined members stay readable.
        assert(guarded.tag == nil and guarded.KIND == 7 and guarded:script() == 'script' and guarded.missing == nil)
        -- A native function becomes one that raises, so no raw callback runs.
        assert(type(guarded.move) == 'function' and guarded.move ~= native_move)
        assert(refused(function() guarded:move(1) end) and refused(function() return guarded:x() end))
        assert(refused(function() return guarded.pos end) and refused(function() guarded.pos = 1 end))
        assert(refused(function() return guarded[2] end) and refused(function() guarded[2] = 1 end))
        guarded.note = 'field'; assert(guarded.note == 'field')
        -- Live objects of a guarded class are unaffected.
        assert(live.move == native_move and live:move(2):x() == 2 and live[1] == 3)
        -- The nearest setting wins: this class hands out the raw function as it is.
        assert(loose.move == native_move and loose.tag == nil)
        local ok, message = pcall(loose.move, loose, 1)
        assert(not ok and message:find('expected a live Node', 1, true))
    )");
    check(guarded.x == 0 && loose.x == 0 && live.x == 2, "no native code ran with an expired receiver");
    check(lua_gettop(L) == 0, "the expired guard keeps the stack balanced");
}

void late_and_changed_bases() {
    state vm;
    binder lua(vm);
    auto* L = vm.get();
    // A base named before it is registered becomes an empty class.
    lua.reg_named_class("Leaf").reg_base("Middle").reg_raw_method("leaf", node_x);
    check(named_class::exists(L, "Middle"), "naming a base registers it");
    Node node;
    check(named_object::push(L, &node, "Leaf"), "expose through the placeholder hierarchy");
    check(named_object::is_a(L, -1, "Middle") && !named_object::is_a(L, -1, "Root"), "placeholder bases are kinds");
    lua_pop(L, 1);
    check(named_object::invalidate(L, &node, "Middle") == 1, "the placeholder base shares the identity cache");
    // Registering the placeholder's own base later reaches existing descendants.
    lua.reg_named_class("Middle").reg_base("Root").reg_raw_method("middle", node_x);
    lua.reg_named_class("Root").reg_raw_method("root", node_x).reg_raw_operator("__eq", node_equal);
    check(named_object::push(L, &node, "Leaf"), "expose after the hierarchy is complete");
    check(named_object::is_a(L, -1, "Root") && named_object::is_a(L, -1, "Middle"), "late bases become kinds");
    lua_getfield(L, -1, "root");
    lua_getfield(L, -2, "middle");
    check(lua_isfunction(L, -1) && lua_isfunction(L, -2), "late base members are inherited");
    lua_pop(L, 2);
    check(named_object::push(L, &node, "Root") && lua_rawequal(L, -1, -2), "the root shares the leaf's cache");
    lua_pop(L, 2);
    check(lua.invalidate_named_object(&node, "Root") == 1, "the root finds wrappers of late descendants");
    lua.reg_named_class("Middle").reg_base("Root");
    fails([&] { lua.reg_named_class("Root").reg_base("Leaf"); }, "derive from itself");
    fails([&] { lua.reg_named_class("Root").reg_base("Root"); }, "derive from itself");
    fails([&] { lua.reg_named_class("Root").reg_base(""); }, "non-empty native class name");
    // A changed base replaces the inherited kinds and members.
    lua.reg_named_class("Other").reg_raw_method("other", node_x);
    lua.reg_named_class("Middle").reg_base("Other");
    check(named_object::push(L, &node, "Leaf"), "expose after the base changed");
    check(named_object::is_a(L, -1, "Other") && !named_object::is_a(L, -1, "Root"), "kinds follow the new base");
    lua_getfield(L, -1, "other");
    lua_getfield(L, -2, "root");
    check(lua_isfunction(L, -2) && lua_isnil(L, -1), "members follow the new base");
    lua_settop(L, 0);
}

void operators() {
    state vm;
    binder lua(vm);
    auto* L = vm.get();
    register_nodes(lua);
    fails([&] { lua.reg_named_class("Node").reg_raw_operator("__index", node_equal); }, "unsupported native operator");
    fails([&] { lua.reg_named_class("Node").reg_raw_operator("__gc", node_equal); }, "unsupported native operator");
    fails([&] { lua.reg_named_class("Node").reg_raw_operator("__eq", nullptr); }, "null operator");
    Sprite a, b;
    Node c;
    set_global(L, "a", &a, "Sprite");
    set_global(L, "b", &b, "Sprite");
    set_global(L, "c", &c, "Node");
    lua.dostring("assert(a ~= b and not pcall(function() return a + b end))");
    // Classes registered before the operator inherit it as well.
    lua.reg_named_class("Node").reg_raw_operator("__eq", node_equal).reg_raw_operator("__add", node_add)
        .reg_raw_operator("__call", node_call);
    lua.reg_named_class("Late").reg_base("Sprite");
    Sprite late;
    set_global(L, "late", &late, "Late");
    lua.dostring(R"(
        assert(not rawequal(a, b) and a == b)
        -- Lua compares with a handler only when both operands share it: same class.
        assert(a ~= c and late ~= a and late == late)
        a:move(2); assert(a ~= b and a + b == 2 and late + a == 2 and c + a == 2)
        assert(engine.Node ~= engine.Sprite and engine.Node == engine.Node)
        assert(not pcall(function() return engine.Node + engine.Sprite end))
        local count, class = engine.Sprite(1, 2)
        assert(count == 3 and class == true)
        count, class = a('x'); assert(count == 2 and class == false)
    )");
    // A derived registration overrides the inherited handler for its subtree.
    lua.reg_named_class("Sprite").reg_raw_operator("__add", +[](lua_State* state) -> int {
        lua_pushliteral(state, "sprite");
        return 1;
    });
    lua.dostring("assert(a + b == 'sprite' and late + late == 'sprite' and c + c == 0)");
    check(lua_gettop(L) == 0, "operators keep the stack balanced");
}

struct NodeRef { Node* value; };
} // namespace

// A host converter turns named objects into typed arguments and results.
namespace arky {
template<> struct converter<NodeRef> {
    static constexpr bool enabled = true;
    static NodeRef read(lua_State* L, int index) {
        if (!named_object::is_a(L, index, "Node")) throw error("expected a Node");
        auto* node = static_cast<Node*>(named_object::pointer(L, index));
        if (!node) throw error("expired Node");
        return {node};
    }
    static void push(lua_State* L, NodeRef value) {
        if (!named_object::push(L, value.value, "Node")) throw error("Node is not registered");
    }
};
} // namespace arky

namespace {
void typed_bridge_and_foreign_values() {
    state vm;
    binder lua(vm);
    auto* L = vm.get();
    register_nodes(lua);
    // Typed callables live next to raw members of the same named class.
    lua.reg_named_class("Node")
        .reg_method("shift", [](NodeRef self, int by) -> NodeRef { self.value->x += by; return self; })
        .reg_method("total", [scale = 10](NodeRef self, NodeRef other) { return (self.value->x + other.value->x) * scale; });
    lua.reg_func("attach", [](NodeRef parent, NodeRef child) { return parent.value->x * 100 + child.value->x; });
    struct Typed { int value = 1; };
    lua.reg_class<Typed>("Typed").reg_con<>().reg_var("value", &Typed::value);
    lua.reg_func("typed_value", [](const Typed& typed) { return typed.value; });
    Sprite sprite;
    Node node;
    set_global(L, "sprite", &sprite, "Sprite");
    set_global(L, "node", &node, "Node");
    lua.dostring(R"(
        assert(rawequal(sprite:shift(4), sprite) and sprite:x() == 4 and node:shift(1):total(sprite) == 50)
        assert(attach(sprite, node) == 401)
        typed = Typed()
    )");
    fails([&] { lua.dostring("attach(sprite, typed)"); }, "expected a Node");
    fails([&] { lua.dostring("attach(sprite, 5)"); }, "expected a Node");
    fails([&] { lua.dostring("sprite:shift('bad')"); }, "invalid arguments to 'shift'");
    lua.dostring("assert(sprite:shift('2'):x() == 6 and sprite:shift(-2):x() == 4)");
    fails([&] { lua.dostring("typed_value(sprite)"); }, "wrong or expired C++ object");
    check(lua.invalidate_named_object(&node, "Node") == 1, "expire a typed argument");
    fails([&] { lua.dostring("attach(sprite, node)"); }, "expired Node");

    // Values the named API does not own are rejected without errors.
    lua_getglobal(L, "typed");
    ::new (lua_newuserdata(L, sizeof(void*) * 2)) void*{&node};
    lua_pushlightuserdata(L, &node);
    lua_newtable(L);
    for (int index = 1; index <= 5; ++index) {
        check(!named_object::pointer(L, index) && !named_object::is_a(L, index, "Node")
              && !named_object::type_name(L, index), "foreign values are not named objects");
        check(!named_object::own(L, index) && !named_object::pin(L, index), "foreign values keep their owner");
    }
    lua_settop(L, 0);
    // Coroutines share classes, identity and fields with the main state.
    lua.dostring(R"(
        local co = coroutine.create(function()
            sprite.from_thread = true
            local made = engine.Sprite.create()
            coroutine.yield(made:move(6):x())
            return rawequal(sprite:shift(1), sprite)
        end)
        local ok, first = coroutine.resume(co); assert(ok and first == 6)
        local done, same = coroutine.resume(co); assert(done and same and sprite.from_thread)
    )");
    check(lua_gettop(L) == 0 && sprite.x == 5, "typed and foreign checks keep the stack balanced");
}

void independent_states() {
    Node shared;
    for (int restart = 0; restart != 3; ++restart) {
        state first, second;
        binder a(first), b(second);
        register_nodes(a);
        b.reg_named_class("Node").reg_raw_method("x", node_x);
        set_global(first.get(), "node", &shared, "Node");
        set_global(second.get(), "node", &shared, "Node");
        a.dostring("node.tag = 'first'; assert(node:move(1) == node)");
        b.dostring("assert(node.tag == nil and node.move == nil and node:x() >= 1)");
        check(a.invalidate_named_object(&shared, "Node") == 1, "each VM tracks its own wrappers");
        b.dostring("assert(node:x() >= 1)");
        check(!named_class::exists(second.get(), "Sprite"), "classes are registered per VM");
    }
    check(shared.x == 3, "every restart reached the same native object");
}
} // namespace

int main() {
    try {
        registration_and_lookup();
        script_extension_and_native_backup();
        identity_and_refinement();
        instance_fields();
        ownership();
        pins_and_invalidation();
        expired_guard();
        late_and_changed_bases();
        operators();
        typed_bridge_and_foreign_values();
        independent_states();
        std::cout << "named_class: " << checks << " checks passed\n";
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
