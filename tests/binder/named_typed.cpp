#include "binder/lua_binder.h"
#include <iostream>
#include <stdexcept>
#include <string>
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
} // namespace

int main() {
    try {
        registration_in_sample_form();
        lua_owned_values();
        owned_objects_of_a_pinned_hierarchy();
        classes_bound_by_name();
        std::cout << "named_typed: " << checks << " checks passed\n";
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
