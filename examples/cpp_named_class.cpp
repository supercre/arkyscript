// Named classes registered in typed form. The classes keep the object model
// that scripts written for raw engine bindings expect: Class:create() factories,
// overloads, chained setters, Lua fields on native objects, and wrappers that
// follow the native object's lifetime.
#include "binder/lua_binder.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>

struct Ref {
    virtual ~Ref() noexcept = default;
};
struct Node : Ref {
    float x = 0, y = 0;
    int tag = 0, order = 0;
    Node* parent = nullptr;
    void setPosition(float px, float py) { x = px; y = py; }
    void addChild(Node* child, int z = 0) { child->parent = this; child->order = z; }
    int getTag() const { return tag; }
    void setTag(int value) { tag = value; }
};
struct Sprite : Node {
    std::string file;
};
// The host owns every object; scripts only refer to them.
struct Scene {
    std::vector<std::unique_ptr<Ref>> objects;
    template<class T> T* make() {
        objects.push_back(std::make_unique<T>());
        return static_cast<T*>(objects.back().get());
    }
};

int main() {
    try {
        arky::state vm;
        arky::binder lua(vm);
        static Scene scene;

        lua.reg_named_class<Ref>("cc.Ref")
            .reg_native_lifetime(true)   // wrappers and their Lua fields live as long as the object
            .enable_expired_guard()
            .publish("cc.Ref");
        lua.reg_named_class<Node>("cc.Node").chain_void_methods()
            .reg_base<Ref>()
            .reg_static("create", [] { return scene.make<Node>(); })
            .reg_method("setPosition", [](Node* self, float x, float y) { self->setPosition(x, y); })
            .reg_method("addChild",
                [](Node* self, Node* child) { self->addChild(child); },
                [](Node* self, Node* child, int z) { self->addChild(child, z); })
            .reg_method("getParent", [](Node* self) { return self->parent; })
            .reg_func("getTag", &Node::getTag)
            .reg_func("setTag", &Node::setTag)
            .reg_var("order", &Node::order)
            .publish("cc.Node");
        lua.reg_named_class<Sprite>("cc.Sprite").chain_void_methods()
            .reg_base<Node>()
            .reg_static("create", [](const std::string& path) {
                auto* sprite = scene.make<Sprite>();
                sprite->file = path;
                return sprite;
            })
            .reg_var("file", &Sprite::file)
            .publish("cc.Sprite");

        lua.dostring(R"(
            local root = cc.Node:create()
            local hero = cc.Sprite:create('hero.png')
            root:addChild(hero:setPosition(10, 20):setTag(42), 3)
            hero.hp = 100                       -- a Lua field on a native object
            assert(rawequal(hero:getParent(), root) and hero.order == 3 and hero.file == 'hero.png')
            function summary() return hero.file .. ': ' .. hero:getTag() .. ' / ' .. hero.hp end
        )");
        const auto summary = lua.call<std::string>("summary");
        // The host announces destruction; stale script references expire.
        for (const auto& object : scene.objects) lua.invalidate_named_object(object.get(), "cc.Ref");
        scene.objects.clear();
        std::cout << summary << '\n';
        return summary == "hero.png: 42 / 100" ? 0 : 1;
    } catch (const arky::error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
