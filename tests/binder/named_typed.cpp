#define _CRT_SECURE_NO_WARNINGS // std::getenv for ARKY_TEST_TRACE
#include "binder/lua_binder.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

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
    throw std::runtime_error("expected a typed named class error");
}

// An engine-shaped hierarchy: natively owned, reference-identified objects.
struct Vec2 { float x = 0, y = 0; };
enum class Align { left, center, right };
struct Ref {
    static inline int live = 0, next = 0;
    int id = ++next, lua_id = 0;
    Ref() { ++live; }
    virtual ~Ref() noexcept { --live; }
    int getId() const noexcept { return id; }
};
struct Node : Ref {
    Vec2 position;
    int tag = 0, order = 0;
    bool visible = true;
    Align align = Align::left;
    std::string name, label;
    Node* parent = nullptr;
    std::vector<Node*> children;
    static Node* create() { return new Node; }
    void setPosition(float x, float y) { position = {x, y}; }
    void setPosition(const Vec2& value) { position = value; }
    const Vec2& getPosition() const { return position; }
    void addChild(Node* child, int z = 0, const std::string& child_name = {}) {
        if (!child) throw std::invalid_argument("child must not be null");
        child->parent = this;
        child->order = z;
        if (!child_name.empty()) child->name = child_name;
        children.push_back(child);
    }
    Node* getParent() const { return parent; }
    Node* getChild(int index) const { return children.at(static_cast<std::size_t>(index)); }
    int getTag() const { return tag; }
    void setTag(int value) { tag = value; }
    const std::string& getName() const { return name; }
    void setName(const std::string& value) { name = value; }
    virtual const char* kind() const { return "node"; }
};
struct Sprite : Node {
    std::string file;
    int frame = 0;
    static Sprite* create() { return new Sprite; }
    static Sprite* create(const std::string& path) { auto* sprite = new Sprite; sprite->file = path; return sprite; }
    const char* kind() const override { return "sprite"; }
};
struct Unbound : Node {};
// A value-like class that Lua owns.
struct Bounds {
    static inline int live = 0;
    float width = 0, height = 0;
    Bounds() { ++live; }
    ~Bounds() noexcept { --live; }
    float area() const { return width * height; }
};
} // namespace

// Host value types convert through named_value; read reports failure by result.
namespace arky {
template<> struct named_value<Vec2> {
    static constexpr bool enabled = true;
    static bool read(lua_State* L, int index, Vec2& out) {
        if (!lua_istable(L, index)) return false;
        lua_getfield(L, index, "x");
        lua_getfield(L, index, "y");
        const bool ok = lua_isnumber(L, -1) && lua_isnumber(L, -2);
        out = {static_cast<float>(lua_tonumber(L, -2)), static_cast<float>(lua_tonumber(L, -1))};
        lua_pop(L, 2);
        return ok;
    }
    static void push(lua_State* L, const Vec2& value) {
        lua_createtable(L, 0, 2);
        lua_pushnumber(L, value.x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, value.y);
        lua_setfield(L, -2, "y");
    }
};
// A pointer type the host passes as a value instead of as an object.
template<> struct named_value<const Bounds*> {
    static constexpr bool enabled = true;
    static bool read(lua_State* L, int index, const Bounds*& out) {
        static Bounds unit;
        if (!lua_isnumber(L, index)) return false;
        unit.width = unit.height = static_cast<float>(lua_tonumber(L, index));
        out = &unit;
        return true;
    }
    static void push(lua_State* L, const Bounds* const& value) {
        if (value) lua_pushnumber(L, value->area()); else lua_pushnil(L);
    }
};
} // namespace arky

namespace {
int raw_children(lua_State* L) {
    auto* node = static_cast<Node*>(named_object::pointer(L, 1));
    if (!node) return luaL_error(L, "expected a live node");
    lua_pushinteger(L, static_cast<lua_Integer>(node->children.size()));
    lua_pushstring(L, node->kind());
    return 2;
}
void register_engine(binder& lua) {
    // The root decides the lifetime of the hierarchy: natively owned, pinned
    // while the object lives, and guarded once it is destroyed.
    lua.reg_named_class<Ref>("cc.Ref")
        .reg_native_lifetime(true, [](Ref* ref) noexcept { ref->lua_id = ref->id; })
        .enable_expired_guard()
        .reg_func("getId", &Ref::getId)
        .publish("cc.Ref");
    lua.reg_named_class<Node>("cc.Node").chain_void_methods()
        .reg_base<Ref>()
        .reg_static("create", [] { return Node::create(); })
        .reg_method("setPosition",
            [](Node* self, float x, float y) { self->setPosition(x, y); },
            [](Node* self, const Vec2& value) { self->setPosition(value); })
        .reg_method("getPosition", [](const Node* self) -> const Vec2& { return self->getPosition(); })
        .reg_method("addChild",
            [](Node* self, Node* child) { self->addChild(child); },
            [](Node* self, Node* child, int z) { self->addChild(child, z); },
            [](Node* self, Node* child, int z, int tag) { self->addChild(child, z); child->tag = tag; },
            [](Node* self, Node* child, int z, const std::string& name) { self->addChild(child, z, name); })
        .reg_method("getParent", [](Node* self) { return self->getParent(); })
        .reg_method("getChild", [](Node& self, int index) -> Node* { return self.getChild(index); })
        .reg_method("isChildOf", [](const Node& self, const Node* other) { return other && self.parent == other; })
        .reg_method("setAlign", [](Node* self, Align value) { self->align = value; })
        .reg_method("getAlign", [](Node* self) { return self->align; })
        .reg_method("setLabel", [](Node* self, const char* text) { self->label = text; })
        .reg_method("setVisible", [](Node* self, bool value) { self->visible = value; })
        .reg_method("kind", [](Node* self) { return self->kind(); })
        .reg_method("fail", [](Node*) { throw std::runtime_error("native failure"); })
        .reg_func("getTag", &Node::getTag)
        .reg_func("setTag", &Node::setTag)
        .reg_prop("name", &Node::getName, &Node::setName)
        .reg_prop("id", &Ref::getId)
        .reg_var("order", &Node::order)
        .reg_raw_method("children", raw_children)
        .publish("cc.Node");
    lua.reg_named_class<Sprite>("cc.Sprite").chain_void_methods()
        .reg_base<Node>()
        .reg_static("create",
            [] { return Sprite::create(); },
            [](const std::string& path) { return Sprite::create(path); })
        .reg_method("getFile", [](Sprite* self) -> const std::string& { return self->file; })
        .reg_var("frame", &Sprite::frame)
        .publish("cc.Sprite");
    lua.reg_named_class<Bounds>("cc.Bounds")
        .reg_static("new", [] { return lua_owned<Bounds>{new Bounds}; })
        .reg_collector(+[](lua_State* L) -> int { delete static_cast<Bounds*>(named_object::pointer(L, 1)); return 0; })
        .reg_var("width", &Bounds::width)
        .reg_var("height", &Bounds::height)
        .reg_func("area", &Bounds::area)
        .publish("cc.Bounds");
}
void destroy(binder& lua, Ref* ref) {
    lua.invalidate_named_object(ref, "cc.Ref");
    delete ref;
}

void registration_in_sample_form() {
    state vm;
    binder lua(vm);
    auto* L = vm.get();
    register_engine(lua);
    lua.dostring(R"lua(
        local parent, child = cc.Node:create(), cc.Sprite:create('hero.png')
        -- void members return their receiver; overloads are chosen by count, then type.
        assert(rawequal(parent:setPosition(3, 4):setTag(7), parent) and parent:getTag() == 7)
        local position = parent:getPosition(); assert(position.x == 3 and position.y == 4)
        parent:setPosition({x = 5, y = 6}); assert(parent:getPosition().y == 6)
        parent:addChild(child, 2, 'hero'); assert(child.name == 'hero' and child.order == 2)
        parent:addChild(cc.Node:create(), 3, 9); assert(parent:getChild(1):getTag() == 9)
        assert(not pcall(parent.addChild, parent, child, 1, {}) and not pcall(parent.setPosition, parent))
        -- Raw Lua coercions: numeric strings, truncation, nil as false or empty text.
        parent:setTag('12'); assert(parent:getTag() == 12)
        parent:setTag(4.9); assert(parent:getTag() == 4)
        parent:setTag(1e30); assert(parent:getTag() == 2147483647)
        parent:setVisible(nil); parent:setLabel(15); parent:setLabel(nil)
        parent:setAlign(2); assert(parent:getAlign() == 2)
        assert(not pcall(parent.setTag, parent, 'text') and not pcall(parent.setVisible, parent, 1))
        -- A base pointer is exposed as the registered class of the object.
        local first = parent:getChild(0)
        assert(rawequal(first, child) and first:getFile() == 'hero.png' and first:kind() == 'sprite')
        stray = cc.Node:create()
        assert(rawequal(child:getParent(), parent) and stray:getParent() == nil)
        assert(child:isChildOf(parent) and not parent:isChildOf(nil) and not pcall(parent.isChildOf, parent, 5))
        -- Properties and member variables, typed and raw members side by side.
        child.name = 'renamed'; child.frame = '3'; child.order = 8
        assert(child.name == 'renamed' and child.frame == 3 and child.order == 8 and child.id == child:getId())
        local count, kind = parent:children(); assert(count == 2 and kind == 'node')
        -- Objects stay scriptable in the named model: fields and per-object overrides.
        child.note = 'field'
        local native = child.setTag
        child.setTag = function(self, value) return native(self, value * 2) end
        assert(child:setTag(5):getTag() == 10)
        child.setTag = nil; assert(child:setTag(5):getTag() == 5)
        -- Failures: a C++ exception, a wrong receiver, a missing overload.
        local ok, message = pcall(parent.fail, parent); assert(not ok and message:find('native failure', 1, true))
        ok, message = pcall(parent.addChild, parent, nil); assert(not ok and message:find('child must not be null', 1, true))
        ok, message = pcall(parent.getTag, {}); assert(not ok and message:find('invalid arguments', 1, true))
        ok, message = pcall(function() local tag = parent:getTag(1); return tag end)
        assert(not ok and message:find("invalid arguments to 'getTag' (1 given)", 1, true))
        ok, message = pcall(cc.Sprite.create, cc.Sprite, 1, 2); assert(not ok and message:find('invalid arguments', 1, true))
        assert(not pcall(child.getFile, parent))
        keep_parent, keep_child = parent, child
    )lua");
    lua_getglobal(L, "keep_child");
    auto* child = static_cast<Sprite*>(named_object::pointer(L, -1));
    lua_getglobal(L, "keep_parent");
    auto* parent = static_cast<Node*>(named_object::pointer(L, -1));
    check(child && parent && child->parent == parent && parent->label.empty() && !parent->visible,
          "typed members reached the native objects");
    check(child->lua_id == child->id && parent->lua_id == parent->id, "the exposure hook ran for each new wrapper");
    check(named_object::is_a(L, -2, "cc.Node") && std::string_view(named_object::type_name(L, -2)) == "cc.Sprite",
          "a bound type keeps its named class");
    lua_settop(L, 0);
    // Typed pushes from host code use the same identity and dynamic type.
    check(named_object::push(L, static_cast<Node*>(child)) && named_object::push(L, static_cast<Ref*>(child)), "push bound pointers");
    lua_getglobal(L, "keep_child");
    check(lua_rawequal(L, -1, -2) && lua_rawequal(L, -1, -3), "every static type yields the same wrapper");
    Unbound unbound;
    check(named_object::push(L, static_cast<Node*>(&unbound)) && std::string_view(named_object::type_name(L, -1)) == "cc.Node",
          "an unregistered dynamic type falls back to the bound static type");
    check(!named_object::push(L, &unbound) && lua_isnil(L, -1), "a static type that is not bound pushes nil");
    lua.invalidate_named_object(static_cast<Ref*>(&unbound), "cc.Ref");
    Bounds loose;
    check(named_object::push(L, static_cast<Bounds*>(nullptr)) && lua_isnil(L, -1), "a null pointer is nil");
    check(named_object::push(L, &loose) && std::string_view(named_object::type_name(L, -1)) == "cc.Bounds",
          "a non-polymorphic type uses its static class");
    lua_settop(L, 0);
    // Pinned wrappers keep their fields without Lua references.
    lua.dostring("keep_parent, keep_child = nil, nil; collectgarbage('collect'); collectgarbage('collect')");
    check(named_object::push(L, child), "expose the pinned object again");
    lua_getfield(L, -1, "note");
    check(lua_isstring(L, -1) && std::string_view(lua_tostring(L, -1)) == "field", "fields survive while the object lives");
    lua_pop(L, 1);
    lua_setglobal(L, "child");
    // Destruction: the wrapper expires, the guard keeps native code away.
    for (Node* grandchild : parent->children) if (grandchild != child) destroy(lua, grandchild);
    destroy(lua, child);
    lua.dostring(R"(
        assert(child.note == nil and type(child.getFile) == 'function')
        local ok, message = pcall(function() return child:getFile() end)
        assert(not ok and message:find('expired native object', 1, true))
        ok, message = pcall(cc.Sprite.getFile, child); assert(not ok and message:find('expired native object', 1, true))
        assert(not pcall(function() return child.name end))
    )");
    // An expired object as an argument arrives as a null pointer, like nil.
    check(named_object::push(L, parent), "expose the parent");
    lua_setglobal(L, "parent");
    lua.dostring("assert(not parent:isChildOf(child)); local ok, message = pcall(parent.addChild, parent, child); assert(not ok and message:find('child must not be null', 1, true))");
    destroy(lua, parent);
    lua_getglobal(L, "stray");
    auto* stray = static_cast<Node*>(named_object::pointer(L, -1));
    lua_pop(L, 1);
    check(stray != nullptr, "an object nobody destroyed is still alive");
    destroy(lua, stray);
    check(Ref::live == 1 && lua_gettop(L) == 0, "only the stack-allocated object remains and the stack is balanced");
}

void lua_owned_values() {
    Bounds::live = 0;
    {
        state vm;
        binder lua(vm);
        register_engine(lua);
        lua.dostring(R"(
            local bounds = cc.Bounds:new()
            bounds.width = 3; bounds.height = '4'
            assert(bounds:area() == 12 and bounds.width == 3)
            bounds = nil; collectgarbage('collect'); collectgarbage('collect')
            kept = cc.Bounds:new()
        )");
        check(Bounds::live == 1, "a collected owner released its object");
        // A static is called on its class; the first argument is never taken for it.
        lua.dostring(R"lua(
            local ok, message = pcall(cc.Bounds.new)
            assert(not ok and message:find("called on a class", 1, true), message)
            assert(not pcall(cc.Bounds.new, 1) and not pcall(cc.Bounds.new, {}) and not pcall(kept.new, kept))
            assert(cc.Bounds.new(cc.Bounds) ~= nil and cc.Bounds.new(cc.Node) ~= nil)
        )lua");
        lua.dostring("kept = nil; collectgarbage('collect'); collectgarbage('collect'); kept = cc.Bounds:new()");
        // Classes can be reopened in typed form; registration stays open.
        lua.reg_named_class<Bounds>("cc.Bounds").reg_method("grow", [](Bounds& self, float by) { self.width += by; });
        lua.dostring("kept:grow(2); assert(kept.width == 2 and kept:grow(1) == nil)");
        state empty;
        binder other(empty);
        fails([&] { other.reg_named_class<Sprite>("cc.Sprite").reg_base<Node>(); }, "not bound to a named class");
        fails([&] { lua.reg_named_class<Node>("cc.Node").reg_method(nullptr, [](Node*) {}); }, "non-empty member name");
    }
    check(Bounds::live == 0, "closing the VM released the remaining owner");
}

// A host that also registers classes by name only (as code written for raw
// bindings does) keeps its own record of which C++ type each one stands for.
struct Widget : Node {
    const char* kind() const override { return "widget"; }
};
const char* widget_class = nullptr;
const char* resolve(lua_State*, const std::type_info& type) noexcept {
    return type == typeid(Widget) ? widget_class : nullptr;
}
// A class of the natively owned hierarchy can still hand single objects to
// Lua: ownership releases the pin, so the collector runs.
struct Request : Ref {
    static inline int released = 0;
};
void owned_objects_of_a_pinned_hierarchy() {
    Ref::live = 0;
    Request::released = 0;
    {
        state vm;
        binder lua(vm);
        register_engine(lua);
        lua_State* L = vm.get();
        lua.reg_named_class<Request>("cc.Request")
            .reg_base<Ref>()
            .reg_collector(+[](lua_State* state) -> int {
                delete static_cast<Request*>(named_object::pointer(state, 1));
                ++Request::released;
                return 0;
            })
            .reg_static("new", [] { return lua_owned<Request>{new Request}; })
            .publish("cc.Request");
        lua.dostring("local a = cc.Request:new(); assert(a:getId() > 0); a = nil; collectgarbage('collect'); collectgarbage('collect')");
        check(Request::released == 1 && Ref::live == 0, "an owned object of a pinned hierarchy is collected");
        // The same through the raw interface: expose, then take ownership.
        auto* request = new Request;
        check(named_object::push(L, request, "cc.Request") && named_object::own(L, -1) && !named_object::own(L, -1),
              "ownership is taken once");
        lua_pop(L, 1);
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        check(Request::released == 2 && Ref::live == 0, "taking ownership releases the pin");
        // Without ownership the wrapper stays for the native owner.
        auto* kept = new Request;
        check(named_object::push(L, kept, "cc.Request"), "expose a natively owned object");
        lua_pop(L, 1);
        lua_gc(L, LUA_GCCOLLECT, 0);
        check(Request::released == 2 && Ref::live == 1, "a natively owned object is not collected");
        lua.invalidate_named_object(kept, "cc.Ref");
        delete kept;
    }
    check(Request::released == 2 && Ref::live == 0, "nothing is released twice");
}
void classes_bound_by_name() {
    Ref::live = 0;
    std::vector<Ref*> created;
    {
        state vm;
        binder lua(vm);
        register_engine(lua);
        lua_State* L = vm.get();
        lua.reg_named_class("ext.Widget").reg_base("cc.Node")
            .reg_raw_method("isWidget", +[](lua_State* state) -> int { lua_pushboolean(state, true); return 1; });
        auto* widget = new Widget;
        created.push_back(widget);
        check(named_class::of_type(L, typeid(Sprite)) == std::string_view("cc.Sprite"), "a typed class answers for its C++ type");
        check(!named_class::of_type(L, typeid(Widget)), "a type nobody bound has no class");
        check(named_object::push(L, static_cast<Node*>(widget)) && std::string_view(named_object::type_name(L, -1)) == "cc.Node",
              "without a resolver the object is exposed as its bound base");
        lua_pop(L, 1);
        lua.invalidate_named_object(widget, "cc.Ref");

        named_class::reg_type_resolver(L, resolve);
        check(!named_class::of_type(L, typeid(Widget)), "the resolver may decline");
        widget_class = "ext.Missing";
        check(!named_class::of_type(L, typeid(Widget)), "an unregistered name is no class");
        widget_class = "ext.Widget";
        check(named_class::of_type(L, typeid(Widget)) == std::string_view("ext.Widget"), "the resolver names the class of a type");
        auto* parent = Node::create();
        created.push_back(parent);
        parent->addChild(widget);
        check(named_object::push(L, parent), "expose the parent");
        lua_setglobal(L, "parent");
        // The typed getter returns Node*; the object arrives as its own class.
        lua.dostring(R"lua(
            local child = parent:getChild(0)
            assert(child:isWidget() and child:kind() == 'widget' and child:getParent() == parent)
            assert(parent:addChild(child, 2) == parent)       -- accepted where a Node is expected
        )lua");
        check(named_object::push(L, static_cast<Ref*>(widget)) && std::string_view(named_object::type_name(L, -1)) == "ext.Widget",
              "every bound base resolves to the same class");
        lua_pop(L, 1);
        named_class::reg_type_resolver(L, nullptr);
        check(!named_class::of_type(L, typeid(Widget)), "the resolver can be removed");
        // One class may stand for a second C++ type: both convert as arguments.
        lua.reg_named_class<Widget>("cc.Sprite");
        auto* second = new Widget;
        created.push_back(second);
        check(named_object::push(L, second) && std::string_view(named_object::type_name(L, -1)) == "cc.Sprite",
              "a second type bound to a class is exposed as that class");
        lua_setglobal(L, "second");
        lua.reg_named_class<Node>("cc.Node")
            .reg_method("takesWidget", [](Node*, Widget* widget) { return widget != nullptr; })
            .reg_method("takesSprite", [](Node*, Sprite* sprite) { return sprite != nullptr; });
        lua.dostring("assert(parent:takesWidget(second) and parent:takesSprite(second) and not pcall(parent.takesWidget, parent, parent))");
        for (Ref* ref : created) lua.invalidate_named_object(ref, "cc.Ref");
    }
    for (Ref* ref : created) delete ref;
    check(Ref::live == 0, "the host released its objects");
}
// Arguments and results that are not one value each: every remaining
// argument, several results, arrays and optional values.
void several_values() {
    const int before = Ref::live;
    std::vector<Ref*> created;
    {
        state vm;
        lua_State* L = vm.get();
        luaL_openlibs(L);
        binder lua(L);
        register_engine(lua);
        auto* parent = new Node;
        created.push_back(parent);
        check(named_object::push(L, parent), "expose the parent");
        lua_setglobal(L, "parent");
        lua.reg_named_class<Node>("cc.Node").chain_void_methods()
            // The last parameter takes what is left, also nothing.
            .reg_method("addChildren", [](Node* self, lua_values<Node*> children) {
                for (Node* child : children) self->addChild(child);
            })
            .reg_method("sum",
                [](Node*, int first) { return first * 100; },                  // an exact overload registered first wins
                [](Node*, int first, const lua_values<double>& rest) {
                    double total = first;
                    for (double value : rest) total += value;
                    return total;
                })
            .reg_static("count", [](lua_values<std::string> names) { return static_cast<int>(names.size()); })
            // One Lua value per tuple element or per item.
            .reg_method("getXY", [](Node* self) { return std::tuple<float, float>(self->position.x, self->position.y); })
            .reg_method("describe", [](Node* self) { return std::tuple<Node*, std::string, bool>(self->parent, self->name, self->visible); })
            .reg_method("tags", [](Node* self) {
                lua_values<int> tags;
                for (Node* child : self->children) tags.push_back(child->tag);
                return tags;
            })
            // Arrays of objects and values, and values that may be absent.
            .reg_method("getChildren", [](Node* self) { return self->children; })
            .reg_method("setChildren", [](Node* self, const std::vector<Node*>& children) {
                self->children.clear();
                for (Node* child : children) self->addChild(child);
            })
            .reg_method("orders", [](Node* self) {
                std::vector<int> orders;
                for (Node* child : self->children) orders.push_back(child->order);
                return orders;
            })
            .reg_method("setOrders", [](Node* self, const std::vector<int>& orders) {
                for (std::size_t i = 0; i < orders.size() && i < self->children.size(); ++i) self->children[i]->order = orders[i];
            })
            .reg_method("firstTag", [](Node* self) -> std::optional<int> {
                if (self->children.empty()) return std::nullopt;
                return self->children.front()->tag;
            })
            .reg_method("moveBy", [](Node* self, std::optional<Vec2> delta) {
                if (delta) self->position = {self->position.x + delta->x, self->position.y + delta->y};
            })
            // Properties from callables with an explicit receiver.
            .reg_prop("childCount", [](const Node* self) { return static_cast<int>(self->children.size()); })
            .reg_prop("x", [](Node* self) { return self->position.x; }, [](Node* self, float x) { self->position.x = x; });
        lua.dostring(R"lua(
            local a, b, c = cc.Sprite:create('a.png'), cc.Node:create(), cc.Node:create()
            a:setTag(1); b:setTag(2); c:setTag(3)
            assert(parent:addChildren() == parent and parent.childCount == 0)
            assert(parent:addChildren(a, b, c) == parent and parent.childCount == 3)
            assert(not pcall(parent.addChildren, parent, a, 'no object'))
            assert(parent:sum(1) == 100 and parent:sum(1, 2, 3.5) == 6.5)
            assert(not pcall(parent.sum, parent) and not pcall(parent.sum, parent, 1, {}))
            assert(cc.Node:count() == 0 and cc.Node:count('a', 'b') == 2 and not pcall(cc.Node.count, 'a'))

            parent:setPosition(3, 4)
            local x, y, extra = parent:getXY()
            assert(x == 3 and y == 4 and extra == nil and select('#', parent:getXY()) == 2)
            local owner, name, visible = a:describe()
            assert(owner == parent and name == '' and visible == true)
            assert(select('#', b:tags()) == 0)
            local t1, t2, t3 = parent:tags()
            assert(t1 == 1 and t2 == 2 and t3 == 3 and select('#', parent:tags()) == 3)

            local children = parent:getChildren()
            assert(#children == 3 and children[1] == a and children[3] == c and children[1]:kind() == 'sprite')
            assert(parent:setChildren({c, a}) == parent and parent.childCount == 2 and parent:getChildren()[1] == c)
            removed = b
            assert(not pcall(parent.setChildren, parent, {a, 1}) and not pcall(parent.setChildren, parent, a))
            parent:setOrders({7, '8'})
            local orders = parent:orders()
            assert(#orders == 2 and orders[1] == 7 and orders[2] == 8)
            assert(not pcall(parent.setOrders, parent, {1, {}}))

            assert(parent:firstTag() == 3 and b:firstTag() == nil and select('#', b:firstTag()) == 1)
            parent:moveBy(nil); assert(parent:getXY() == 3)
            parent:moveBy({x = 1, y = 1}); assert(parent:getXY() == 4)
            assert(not pcall(parent.moveBy, parent, 5) and not pcall(parent.moveBy, parent))

            assert(parent.x == 4)
            parent.x = 10
            assert(parent.x == 10 and parent:getXY() == 10)
            parent.childCount = 9                       -- no setter: an instance field
            assert(parent.childCount == 9)
        )lua");
        // A null object would end the array.
        parent->children.insert(parent->children.begin() + 1, nullptr);
        lua.dostring("local list = parent:getChildren(); assert(#list == 2 and list[1] and list[2])");
        parent->children.erase(parent->children.begin() + 1);
        for (Node* child : parent->children) created.push_back(child);
        lua_getglobal(L, "removed");
        check(named_object::is_a(L, -1, "cc.Node") && lua_gettop(L) == 1, "the stack is balanced");
        created.push_back(static_cast<Node*>(named_object::pointer(L, -1)));
        lua_pop(L, 1);
        for (Ref* ref : created) lua.invalidate_named_object(ref, "cc.Ref");
    }
    for (Ref* ref : created) delete ref;
    check(Ref::live == before, "the host released its objects");
}

// Plain functions with the conversions of typed members.
void functions_with_named_conversions() {
    const int before = Ref::live;
    std::vector<Ref*> created;
    {
        state vm;
        lua_State* L = vm.get();
        luaL_openlibs(L);
        binder lua(L);
        register_engine(lua);
        auto* node = new Sprite;
        created.push_back(node);
        check(named_object::push(L, static_cast<Node*>(node)), "expose a node");
        lua_setglobal(L, "node");
        lua.reg_named_func("util.kindOf", [](Node* value) { return value ? value->kind() : "nil"; });
        lua.reg_named_func("util.distance",
            [](const Vec2& a, const Vec2& b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y); },
            [](float ax, float ay, float bx, float by) { return std::abs(ax - bx) + std::abs(ay - by); });
        lua.reg_named_func("util.origin", [] { return Vec2{1, 2}; });
        lua.reg_named_func("util.pick", [](int index, lua_values<Node*> nodes) -> Node* {
            return index >= 1 && static_cast<std::size_t>(index) <= nodes.size() ? nodes[static_cast<std::size_t>(index - 1)] : nullptr;
        });
        lua.reg_named_func("util.fail", [] { throw std::runtime_error("native failure"); });
        // const Bounds* has its own conversion; Bounds* is still an object.
        lua.reg_named_func("util.area", [](const Bounds* bounds) { return bounds; });
        lua.reg_named_func("util.width", [](Bounds* bounds) { return bounds ? bounds->width : -1.0f; });
        lua.set_namespace("math2");
        lua.reg_named_func("twice", [](double value) { return value * 2; });
        lua.set_namespace("");
        lua.dostring(R"lua(
            assert(util.kindOf(node) == 'sprite' and util.kindOf(nil) == 'nil')
            assert(util.distance({x = 0, y = 0}, {x = 3, y = 4}) == 7 and util.distance(0, 0, 1, 1) == 2)
            local origin = util.origin()
            assert(origin.x == 1 and origin.y == 2 and select('#', util.origin()) == 1)
            assert(util.pick(1, node) == node and util.pick(2, node) == nil and util.pick(1) == nil)
            assert(math2.twice(4) == 8)
            local ok, message = pcall(util.distance, 1, 2, 3)
            assert(not ok and message:find("invalid arguments to", 1, true) and message:find('(3 given)', 1, true), message)
            ok, message = pcall(util.kindOf, 'text')
            assert(not ok and message:find('(1 given)', 1, true), message)
            ok, message = pcall(util.fail)
            assert(not ok and message:find('native failure', 1, true), message)
            assert(not pcall(util.origin, 1))
            local bounds = cc.Bounds:new()
            bounds.width = 3
            assert(util.area(4) == 16 and not pcall(util.area, bounds))
            assert(util.width(bounds) == 3 and util.width(nil) == -1 and not pcall(util.width, 4))
        )lua");
        check(lua_gettop(L) == 0, "the stack is balanced");
        for (Ref* ref : created) lua.invalidate_named_object(ref, "cc.Ref");
    }
    for (Ref* ref : created) delete ref;
    check(Ref::live == before, "the host released its objects");
}
// lua_rest takes the remaining arguments where they are; lua_pushed returns
// the values a callable pushed itself. Neither converts or allocates.
void values_left_on_the_stack() {
    const int before = Ref::live;
    std::vector<Ref*> created;
    {
        state vm;
        lua_State* L = vm.get();
        luaL_openlibs(L);
        binder lua(L);
        register_engine(lua);
        auto* node = new Node;
        created.push_back(node);
        check(named_object::push(L, node), "expose a node");
        lua_setglobal(L, "node");
        lua.reg_named_class<Node>("cc.Node").reg_method("extras", [](Node*, int base, lua_rest rest) {
            return base + static_cast<int>(rest.size());
        });
        lua.reg_named_func("util.count", [](lua_rest rest) { return static_cast<int>(rest.size()); });
        lua.reg_named_func("util.kinds", [](lua_rest rest) {
            std::string kinds;
            for (std::size_t i = 0; i < rest.size(); ++i) kinds += lua_typename(rest.state, lua_type(rest.state, rest.index(i)))[0];
            return kinds;
        });
        lua.reg_named_func("util.swap", [](lua_rest rest) -> lua_pushed {
            if (rest.size() < 2) return {};
            lua_pushvalue(rest.state, rest.index(1));
            lua_pushvalue(rest.state, rest.index(0));
            return {2};
        });
        lua.reg_named_func("util.broken", [] { return lua_pushed{3}; });
        lua.dostring(R"lua(
            assert(util.count() == 0 and util.count(1, nil, 'x') == 3)
            assert(util.kinds(1, 'a', {}, nil, true) == 'nstnb')
            assert(node:extras(10) == 10 and node:extras(10, 'a', {}, nil) == 13)
            assert(not pcall(node.extras, node))
            local a, b = util.swap(1, 'x')
            assert(a == 'x' and b == 1)
            assert(select('#', util.swap(1)) == 0 and select('#', util.swap(1, 2, 3)) == 2)
            local ok, message = pcall(util.broken)
            assert(not ok and message:find('not on the stack', 1, true), message)
        )lua");
        check(lua_gettop(L) == 0, "the stack is balanced");
        for (Ref* ref : created) lua.invalidate_named_object(ref, "cc.Ref");
    }
    for (Ref* ref : created) delete ref;
    check(Ref::live == before, "the host released its objects");
}

// Raw callbacks read their arguments with the conversions of typed parameters.
int raw_describe(lua_State* L) {
    Node* node = nullptr;
    int count = 0;
    Vec2 offset{};
    if (!named_read(L, 1, node) || !named_read(L, 2, count) || !named_read(L, 3, offset)) return 0;
    lua_pushstring(L, node ? node->kind() : "nil");
    lua_pushnumber(L, count + offset.x + offset.y);
    return 2;
}

void typed_reads_in_raw_callbacks() {
    const int before = Ref::live;
    std::vector<Ref*> created;
    {
        state vm;
        lua_State* L = vm.get();
        luaL_openlibs(L);
        binder lua(L);
        register_engine(lua);
        auto* node = new Sprite;
        created.push_back(node);
        check(named_object::push(L, static_cast<Node*>(node)), "expose a node");
        lua_setglobal(L, "node");
        auto* gone = new Node;
        check(named_object::push(L, gone), "expose a node that is destroyed below");
        lua_setglobal(L, "gone");
        lua.invalidate_named_object(gone, "cc.Ref");
        delete gone;
        lua.reg_func("describe", raw_describe);
        lua.dostring(R"lua(
            local kind, sum = describe(node, 1, {x = 2, y = 3})
            assert(kind == 'sprite' and sum == 6)
            -- nil and a destroyed object arrive as nullptr, a numeric string as a number
            assert(describe(nil, '4', {x = 0, y = 0}) == 'nil' and describe(gone, 0, {x = 0, y = 0}) == 'nil')
            -- a value of another type does not convert
            assert(select('#', describe(cc.Bounds:new(), 1, {x = 0, y = 0})) == 0)
            assert(select('#', describe(node, {}, {x = 0, y = 0})) == 0 and select('#', describe(node, 1, 5)) == 0)
        )lua");
        std::string text;
        std::vector<int> numbers;
        check(luaL_dostring(L, "return 'name', {1, 2, 3}") == 0, "push a string and an array");
        check(named_read(L, -2, text) && text == "name", "named_read converts a string");
        check(named_read(L, -1, numbers) && numbers.size() == 3 && numbers[2] == 3, "named_read converts an array");
        check(!named_read(L, -2, numbers), "named_read reports a value that does not convert");
        lua_settop(L, 0);
        for (Ref* ref : created) lua.invalidate_named_object(ref, "cc.Ref");
    }
    for (Ref* ref : created) delete ref;
    check(Ref::live == before, "the host released its objects");
}
} // namespace

int main() {
    // With ARKY_TEST_TRACE set, each part is named before it runs, so a crash shows where it happened.
    const bool trace = std::getenv("ARKY_TEST_TRACE") != nullptr;
    const auto run = [trace](const char* name, void (*part)()) {
        if (trace) std::cerr << "named_typed: " << name << std::endl;
        part();
    };
    try {
        run("registration_in_sample_form", registration_in_sample_form);
        run("lua_owned_values", lua_owned_values);
        run("owned_objects_of_a_pinned_hierarchy", owned_objects_of_a_pinned_hierarchy);
        run("classes_bound_by_name", classes_bound_by_name);
        run("several_values", several_values);
        run("functions_with_named_conversions", functions_with_named_conversions);
        run("values_left_on_the_stack", values_left_on_the_stack);
        run("typed_reads_in_raw_callbacks", typed_reads_in_raw_callbacks);
        std::cout << "named_typed: " << checks << " checks passed\n";
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
