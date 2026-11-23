// The host runtime the way an application embeds it: one translation unit, no shared modules,
// modules supplied by the host. Runs under the same sanitizers as the binder tests, including
// Lua states closed while worker tasks and file reads are still running.
#include "runtime/arky_runtime.cpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>
#include <map>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void fails(F&& call, const char* expected) {
    try {
        call();
    } catch (const std::exception& failure) {
        if (std::string(failure.what()).find(expected) == std::string::npos)
            throw std::runtime_error(std::string("unexpected failure: ") + failure.what());
        return;
    }
    throw std::runtime_error(std::string("expected a failure: ") + expected);
}
void run(lua_State* L, const char* code) {
    if (luaL_dostring(L, code) == 0) return;
    const char* text = lua_tostring(L, -1);
    std::string message = text ? text : "error object is not a string";
    lua_pop(L, 1);
    throw std::runtime_error(message);
}
// Steps once per "frame" until no task is pending.
int frames_until_idle(lua_State* L, int max_resumes = 64) {
    const auto end = std::chrono::steady_clock::now() + 30s;
    int frames = 1;
    while (arky::runtime::step(L, max_resumes)) {
        ++frames;
        check(std::chrono::steady_clock::now() < end, "tasks did not finish");
        std::this_thread::sleep_for(1ms);
    }
    return frames;
}

// What the host of these tests has in place of files: module name -> generated Lua.
struct project {
    std::map<std::string, std::string, std::less<>> modules;
    std::map<std::string, int, std::less<>> loads;

    arky::runtime::options options() {
        arky::runtime::options settings;
        settings.import_loader = [this](std::string_view name, arky::runtime::module_source& module) {
            if (name == "explode") throw std::runtime_error("disk on fire");
            const auto found = modules.find(name);
            if (found == modules.end()) return false;
            ++loads[std::string(name)];
            module.chunk = found->second;
            if (name != "unnamed") module.name = "project/" + std::string(name) + ".lua";
            return true;
        };
        return settings;
    }
};

// The same shape the compiler emits: helpers come from the chunk's environment.
constexpr const char* generated_main = R"lua(
local __tynex_import = assert(__tynex_import, 'static import requires the Tynex project loader')
local __tynex_async_runtime = assert(__tynex_async_runtime, 'async requires the Tynex async runtime')
local util = __tynex_import("util")
assert(util == __tynex_import("util") and util.name == "util")
assert(type(fs.read_file) == "function" and type(net.connect) == "function")
local double = __tynex_async_runtime.create(function(n)
   __tynex_async_runtime.await(scheduler.sleep(0))   -- the next frame
   return util.double(n)
end)
local main = __tynex_async_runtime.create(function()
   local total = 0
   for i = 1, 3 do total = total + __tynex_async_runtime.await(double(i)) end
   return total
end)
started = main()
-- main ran here, up to its first await: it waits for a child, which waits for its timer.
assert(scheduler.pending() == 3 and started:status() == "waiting")
assert(not pcall(function() scheduler.step = nil end))
return {module = ..., task = started}
)lua";

void lifecycle() {
    arky::state vm;
    lua_State* L = vm.get();
    check(!arky::runtime::is_open(L) && !arky::runtime::is_open(nullptr), "a new Lua state has no runtime");
    fails([&] { arky::runtime::step(L); }, "not open");
    fails([&] { arky::runtime::import_module(L, "main"); }, "not open");
    fails([] { arky::runtime::open(nullptr); }, "live Lua state");
    arky::runtime::close(L);
    arky::runtime::close(nullptr);

    arky::runtime::open(L);
    check(arky::runtime::is_open(L), "open installs the runtime");
    fails([&] { arky::runtime::open(L); }, "already open");
    check(!arky::runtime::step(L), "an idle runtime has nothing pending");
    fails([&] { arky::runtime::step(L, 0); }, "integer budget");
    fails([&] { arky::runtime::import_module(L, "main"); }, "unprepared static import 'main'");
    // The runtime is reached through the environment of generated chunks, not through globals.
    run(L, "assert(scheduler == nil and fs == nil and net == nil)");
    run(L, "assert(__tynex_import == nil and __tynex_async_runtime == nil)");
    run(L, "assert(type(require('tynex_task').spawn) == 'function')");
    run(L, "assert(type(require('tynex_io').read_file) == 'function')");
    check(lua_gettop(L) == 0, "the runtime leaves the caller's stack as it was");

    arky::runtime::close(L);
    arky::runtime::close(L);
    check(!arky::runtime::is_open(L), "close forgets the runtime");
    run(L, "assert(package.loaded.tynex_task == nil and package.loaded.tynex_io == nil)");
    fails([&] { arky::runtime::step(L); }, "not open");

    // A module the host loaded under the same name is left alone.
    run(L, "package.loaded.tynex_task = 'host module'");
    arky::runtime::open(L);
    run(L, "assert(package.loaded.tynex_task == 'host module' and type(package.loaded.tynex_io) == 'table')");
    arky::runtime::close(L);
    run(L, "assert(package.loaded.tynex_task == 'host module' and package.loaded.tynex_io == nil)");
    check(lua_gettop(L) == 0, "the runtime leaves the caller's stack as it was");
}

void imports() {
    project files;
    files.modules["util"] = "return {double = function(n) return n * 2 end, name = ...}";
    files.modules["main"] = generated_main;
    files.modules["silent"] = "local unused = 1";
    files.modules["unnamed"] = "return debug.getinfo(1, 'S').source";
    files.modules["broken"] = "return +";
    files.modules["a"] = "return __tynex_import('b')";
    files.modules["b"] = "return __tynex_import('a')";
    files.modules["thrower"] = "error({code = 7})";
    files.modules["catcher"] = "local ok, value = pcall(__tynex_import, 'thrower')\n"
                               "return {ok = ok, code = type(value) == 'table' and value.code}";
    arky::state vm;
    lua_State* L = vm.get();
    arky::runtime::open(L, files.options());

    arky::runtime::import_module(L, "main");
    check(lua_gettop(L) == 1 && lua_istable(L, -1), "import_module pushes the module's value");
    lua_getfield(L, -1, "module");
    check(std::string(lua_tostring(L, -1)) == "main", "a module receives its name");
    lua_pop(L, 1);
    arky::runtime::import_module(L, "main");
    check(lua_rawequal(L, -1, -2) != 0, "a module is run once");
    lua_pop(L, 2);
    check(files.loads["main"] == 1 && files.loads["util"] == 1, "each module is read once");

    // One resume per frame: a child once its timer is due, then main, three times.
    check(frames_until_idle(L, 1) == 6, "the scheduler advances only when the host steps it");
    run(L, "assert(started:status() == 'succeeded' and started:result() == 12)");

    arky::runtime::import_module(L, "silent");
    check(lua_isboolean(L, -1) && lua_toboolean(L, -1), "a module without a value is true");
    lua_pop(L, 1);
    arky::runtime::import_module(L, "unnamed");
    check(std::string(lua_tostring(L, -1)) == "@unnamed", "the module name stands in for a missing chunk name");
    lua_pop(L, 1);

    fails([&] { arky::runtime::import_module(L, "missing"); }, "unprepared static import 'missing'");
    fails([&] { arky::runtime::import_module(L, "explode"); }, "unprepared static import 'explode': disk on fire");
    fails([&] { arky::runtime::import_module(L, "broken"); }, "project/broken.lua:1:");
    fails([&] { arky::runtime::import_module(L, "a"); }, "cyclic static import 'a'");
    fails([&] { arky::runtime::import_module(L, "a"); }, "cyclic static import 'a'");

    // A failed module is not cached, and its error value reaches the importing code unchanged.
    arky::runtime::import_module(L, "catcher");
    lua_getfield(L, -1, "ok");
    lua_getfield(L, -2, "code");
    check(!lua_toboolean(L, -2) && lua_tointeger(L, -1) == 7, "an import keeps the module's error value");
    lua_pop(L, 3);
    fails([&] { arky::runtime::import_module(L, "thrower"); }, "error object is not a string");
    check(files.loads["thrower"] == 2, "a failed module is read again");

    // Hosts may ship bytecode instead of source text.
    run(L, "bytecode = string.dump(function(...) return 'compiled:' .. ... end)");
    lua_getglobal(L, "bytecode");
    std::size_t size = 0;
    const char* bytes = lua_tolstring(L, -1, &size);
    files.modules["binary"] = std::string(bytes, size);
    lua_pop(L, 1);
    arky::runtime::import_module(L, "binary");
    check(std::string(lua_tostring(L, -1)) == "compiled:binary", "a module may be LuaJIT bytecode");
    lua_pop(L, 1);
    check(lua_gettop(L) == 0, "the runtime leaves the caller's stack as it was");
}

void workers() {
    project files;
    // A legacy wrapper as in examples/tasks: require returns the module compiled into the runtime.
    files.modules["task"] = "return require('tynex_task')";
    files.modules["jobs"] = R"lua(
local task = __tynex_import("task")
task.configure(2, 16)
local function calculate(input, context)
   local total = 0
   for i = 1, input do
      if i % 1000 == 0 then context.checkpoint() end
      total = total + i
   end
   return total
end
local main = __tynex_async_runtime.create(function()
   local first = scheduler.from_worker(task.spawn(calculate, 10000))
   local second = scheduler.from_worker(task.spawn(calculate, 20000))
   return __tynex_async_runtime.await(first) + __tynex_async_runtime.await(second)
end)
worker_sum = main()
)lua";
    arky::state vm;
    lua_State* L = vm.get();
    arky::runtime::open(L, files.options());
    arky::runtime::import_module(L, "jobs");
    lua_pop(L, 1);
    frames_until_idle(L);
    run(L, "assert(worker_sum:result() == 250015000)");
}

void file_reads() {
    constexpr const char* reads = R"lua(
local load = __tynex_async_runtime.create(function(path, limit)
   return __tynex_async_runtime.await(fs.read_file(path, limit))
end)
return function(path, limit) return load(path, limit) end
)lua";
    constexpr const char* outcome = R"lua(
function outcome(task)
   local ok, value = pcall(task.result, task)
   return task:status() .. ":" .. tostring(value)
end
)lua";
    {
        // Without a reader: regular files of the operating system.
        project files;
        files.modules["reads"] = reads;
        arky::state vm;
        lua_State* L = vm.get();
        arky::runtime::open(L, files.options());
        run(L, outcome);
        arky::runtime::import_module(L, "reads");
        lua_setglobal(L, "read");
        run(L, "text = read('tests/runtime/fixture.txt') missing = read('tests/runtime/no-such-file.txt')");
        frames_until_idle(L);
        // A Windows checkout may convert the fixture's line ending.
        run(L, "assert(outcome(text):find('^succeeded:arky runtime fixture\\r?\\n$'), outcome(text))");
        run(L, "assert(outcome(missing):find('^failed:cannot open file'), outcome(missing))");
    }
    {
        // With a reader: every read goes to the host, on a worker thread.
        project files;
        files.modules["reads"] = reads;
        std::atomic<int> calls{0};
        std::atomic<bool> on_lua_thread{false};
        const auto lua_thread = std::this_thread::get_id();
        auto settings = files.options();
        settings.file_reader = [&](std::string_view path, std::size_t max_bytes, std::string& bytes, std::string& error) {
            ++calls;
            if (std::this_thread::get_id() == lua_thread) on_lua_thread = true;
            if (path == "pack://hello") { bytes = "from the pack"; return true; }
            if (path == "pack://big") { bytes.assign(max_bytes + 1, 'x'); return true; }
            if (path == "pack://throw") throw std::runtime_error("reader failed");
            if (path == "pack://silent") return false;
            error = "no such entry: " + std::string(path);
            return false;
        };
        arky::state vm;
        lua_State* L = vm.get();
        arky::runtime::open(L, settings);
        run(L, outcome);
        arky::runtime::import_module(L, "reads");
        lua_setglobal(L, "read");
        run(L, "hello = read('pack://hello') big = read('pack://big', 16) thrown = read('pack://throw')");
        run(L, "silent = read('pack://silent') regular = read('tests/runtime/fixture.txt')");
        frames_until_idle(L);
        run(L, "assert(outcome(hello) == 'succeeded:from the pack', outcome(hello))");
        run(L, "assert(outcome(big) == 'failed:file exceeds max_bytes', outcome(big))");
        run(L, "assert(outcome(thrown) == 'failed:reader failed', outcome(thrown))");
        run(L, "assert(outcome(silent) == 'failed:cannot read file', outcome(silent))");
        run(L, "assert(outcome(regular) == 'failed:no such entry: tests/runtime/fixture.txt', outcome(regular))");
        check(calls == 5 && !on_lua_thread, "the host reader runs on worker threads only");
    }
}

// Host time is what the host passes to step; real time is the clock.
void timers() {
    project files;
    files.modules["waits"] = R"lua(
local wait = __tynex_async_runtime.create(function(milliseconds, realtime)
   __tynex_async_runtime.await((realtime and scheduler.sleep_realtime or scheduler.sleep)(milliseconds))
   return milliseconds
end)
game = wait(1000)
frame = wait(0)
real = wait(20, true)
)lua";
    arky::state vm;
    lua_State* L = vm.get();
    arky::runtime::open(L, files.options());
    arky::runtime::import_module(L, "waits");
    lua_pop(L, 1);
    // The host passes seconds; scripts count in milliseconds.
    fails([&] { arky::runtime::step(L, -1.0); }, "expects delta_seconds");
    fails([&] { arky::runtime::step(L, std::numeric_limits<double>::infinity()); }, "expects delta_seconds");
    fails([&] { arky::runtime::step(L, std::numeric_limits<double>::quiet_NaN()); }, "expects delta_seconds");
    // The functions started at their calls and wait; no timer completes before a step.
    run(L, "assert(game:status() == 'waiting' and frame:status() == 'waiting' and real:status() == 'waiting')");
    check(arky::runtime::step(L, 0.0), "no time has passed for the game");
    run(L, "assert(frame:result() == 0 and game:status() == 'waiting')");
    check(arky::runtime::step(L, 0.4f), "a float frame time selects the overload with elapsed seconds");
    run(L, "assert(game:status() == 'waiting')");
    check(arky::runtime::step(L, 0.4, 8), "host time has not reached the timer");
    run(L, "assert(game:status() == 'waiting')");
    arky::runtime::step(L, 0.4);
    run(L, "assert(game:result() == 1000)");
    // A paused game passes no time; the real-time timer completes all the same.
    const auto end = std::chrono::steady_clock::now() + 30s;
    while (arky::runtime::step(L, 0.0)) {
        check(std::chrono::steady_clock::now() < end, "the real-time timer did not complete");
        std::this_thread::sleep_for(1ms);
    }
    run(L, "assert(real:result() == 20)");
    // Without elapsed time host time follows the clock.
    files.modules["later"] = "later = __tynex_async_runtime.create(function() __tynex_async_runtime.await(scheduler.sleep(20)) end)()";
    arky::runtime::import_module(L, "later");
    lua_pop(L, 1);
    frames_until_idle(L);
    run(L, "assert(later:status() == 'succeeded')");
    check(lua_gettop(L) == 0, "the runtime leaves the caller's stack as it was");
}

// A host function between two async functions: it calls its first argument with the others.
int relay(lua_State* L) {
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

// An async function starts inside its caller, as a coroutine resumed within it. Past a fixed depth
// a call is queued instead, so recursion as deep as this must neither exhaust the C stack (1 MB for
// the main thread on Windows) nor fail, also when a host function sits between every two levels.
void recursion() {
    project files;
    files.modules["deep"] = R"lua(
local count
count = __tynex_async_runtime.create(function(n)
   if n == 0 then return 0 end
   return 1 + __tynex_async_runtime.await(count(n - 1))
end)
local through
through = __tynex_async_runtime.create(function(n)
   if n == 0 then return 0 end
   return 1 + __tynex_async_runtime.await(relay(through, n - 1))
end)
deep, relayed = count(20000), through(5000)
)lua";
    arky::state vm;
    lua_State* L = vm.get();
    lua_pushcfunction(L, relay);
    lua_setglobal(L, "relay");
    arky::runtime::open(L, files.options());
    arky::runtime::import_module(L, "deep");
    lua_pop(L, 1);
    run(L, "assert(deep:status() == 'waiting' and relayed:status() == 'waiting')");
    frames_until_idle(L, 4096);
    run(L, "assert(deep:result() == 20000 and relayed:result() == 5000)");
    check(lua_gettop(L) == 0, "the runtime leaves the caller's stack as it was");
}

// A bound function which answers later: it returns the Task and keeps the handle.
std::vector<arky::runtime::deferred> requests;
int request(lua_State* L) {
    requests.emplace_back(L);
    return 1;
}
// A bound function which has its answer at once (a cache hit, a rejected argument): it completes
// the Task before returning it, while the script which called it is still running.
int instant(lua_State* L) {
    const double value = lua_tonumber(L, 1);
    const arky::runtime::deferred done(L);
    check(value < 0 ? done.reject("negative") : done.resolve(value * 2),
          "a handle completes its Task inside the function which created it");
    return 1;
}

void deferred_tasks() {
    arky::runtime::deferred outlived;
    {
        project files;
        files.modules["awaits"] = R"lua(
local wait = __tynex_async_runtime.create(function(task) return __tynex_async_runtime.await(task) end)
local ask = __tynex_async_runtime.create(function() return __tynex_async_runtime.await(request()) end)
local sum = __tynex_async_runtime.create(function(count)
   local total = 0
   for i = 1, count do total = total + __tynex_async_runtime.await(instant(i)) end
   return total
end)
return {wait = function(task) return wait(task) end, ask = function() return ask() end,
   sum = function(count) return sum(count) end}
)lua";
        arky::state vm;
        lua_State* L = vm.get();
        fails([&] { arky::runtime::deferred closed(L); }, "not open");
        arky::runtime::open(L, files.options());
        arky::runtime::import_module(L, "awaits");
        lua_setglobal(L, "awaits");

        const arky::runtime::deferred empty;
        check(!empty.alive() && !empty.resolve(1) && !empty.reject("nothing"), "an empty handle does nothing");

        arky::runtime::deferred done(L);
        check(lua_gettop(L) == 1 && done.alive(), "a deferred handle pushes its Task");
        lua_setglobal(L, "answer");
        run(L, "assert(answer:status() == 'waiting') waiter = awaits.wait(answer)");
        check(arky::runtime::step(L), "the waiter waits for the host");
        const arky::runtime::deferred copy = done;
        check(copy.resolve(std::string("body"), 200), "the first resolve completes the Task");
        check(!done.resolve(1) && !done.reject("late"), "a Task is completed once");
        check(!arky::runtime::step(L), "the waiter resumes at the next step");
        run(L, "local body, code = waiter:result() assert(body == 'body' and code == 200)");

        arky::runtime::deferred failing(L);
        lua_setglobal(L, "broken");
        check(failing.reject("no route") && !failing.resolve(), "reject fails the Task");
        run(L, "local ok, message = pcall(broken.result, broken) assert(not ok and message == 'no route')");

        arky::runtime::deferred cancelled(L);
        lua_setglobal(L, "dropped");
        run(L, "assert(dropped:cancel())");
        check(!cancelled.resolve(1) && cancelled.alive(), "a cancelled Task refuses a late result");

        // Created inside an async function: that coroutine is suspended when the answer arrives.
        lua_pushcfunction(L, (arky::detail::boundary<request>));
        lua_setglobal(L, "request");
        run(L, "first = awaits.ask() second = awaits.ask()");
        check(arky::runtime::step(L) && requests.size() == 2, "both requests are waiting for the host");
        check(requests[1].resolve(2.5) && requests[0].resolve(true), "answers may arrive in any order");
        check(!arky::runtime::step(L), "the askers resume");
        run(L, "assert(first:result() == true and second:result() == 2.5)");

        // Created in a coroutine which has ended and been collected since: the handle must not
        // depend on that coroutine's stack.
        run(L, "coroutine.wrap(function() orphan = request() end)() collectgarbage() collectgarbage()");
        check(requests.size() == 3 && requests[2].resolve("kept"), "a handle outlives the coroutine which made it");
        run(L, "collectgarbage() assert(orphan:result() == 'kept')");

        // Completed inside the bound function, from plain code and from an async function whose
        // loop runs long enough to be compiled: the completion runs on the runtime's own coroutine
        // while the caller's is in the middle of a call.
        lua_pushcfunction(L, (arky::detail::boundary<instant>));
        lua_setglobal(L, "instant");
        run(L, "local task = instant(21) assert(task:result() == 42)");
        run(L, "local task = instant(-1) local ok, message = pcall(task.result, task) assert(not ok and message == 'negative')");
        run(L, "total = awaits.sum(2000)");
        frames_until_idle(L);
        run(L, "assert(total:result() == 4002000)");

        // Closing the runtime ends its handles: a late answer does nothing, in that runtime or in
        // the next one of the same Lua state.
        const arky::runtime::deferred unanswered(L);
        lua_setglobal(L, "unanswered");
        arky::runtime::close(L);
        check(!unanswered.alive() && !unanswered.resolve(1) && !unanswered.reject("late"), "a closed runtime's handle does nothing");
        check(!requests[0].alive() && !requests[0].resolve(1), "a closed runtime's handle does nothing");
        run(L, "assert(unanswered:status() == 'waiting')");
        outlived = arky::runtime::deferred();
        arky::runtime::open(L, files.options());
        outlived = arky::runtime::deferred(L);
        lua_pop(L, 1);
        check(outlived.alive() && !unanswered.alive() && !unanswered.resolve(1), "a reopened runtime does not revive old handles");
        check(lua_gettop(L) == 0, "the runtime leaves the caller's stack as it was");
    }
    // The Lua state is gone: late callbacks and the handles' destructors must be harmless.
    check(!outlived.alive() && !outlived.resolve(1) && !outlived.reject("late"), "a handle outlives its Lua state safely");
    check(!requests[0].alive() && !requests[1].resolve("late"), "a handle outlives its Lua state safely");
    requests.clear();
}

// Every Lua state has a runtime of its own: nothing is shared, and one can go while another runs.
void isolation() {
    constexpr const char* counter = R"lua(
local wait = __tynex_async_runtime.create(function()
   __tynex_async_runtime.await(scheduler.sleep(0))
   return "ready"
end)
local main = __tynex_async_runtime.create(function() return __tynex_async_runtime.await(wait()) end)
started = main()
return ...
)lua";
    project first_files;
    project second_files;
    first_files.modules["main"] = counter;
    second_files.modules["main"] = counter;
    arky::state second;
    arky::runtime::open(second.get(), second_files.options());
    {
        arky::state first;
        arky::runtime::open(first.get(), first_files.options());
        arky::runtime::import_module(first.get(), "main");
        arky::runtime::import_module(second.get(), "main");
        lua_pop(first.get(), 1);
        lua_pop(second.get(), 1);
        frames_until_idle(first.get());
        run(first.get(), "assert(started:status() == 'succeeded')");
        run(second.get(), "assert(started:status() == 'waiting')");
        check(first_files.loads["main"] == 1 && second_files.loads["main"] == 1, "each runtime has its own modules");
    }
    frames_until_idle(second.get());
    run(second.get(), "assert(started:result() == 'ready')");
}

// A game restarts its scripts: the runtime is closed and opened again, in the same Lua state or a new one.
void restart() {
    project files;
    files.modules["main"] = "generation = (generation or 0) + 1 return generation";
    arky::state vm;
    lua_State* L = vm.get();
    for (int round = 1; round <= 3; ++round) {
        arky::runtime::open(L, files.options());
        arky::runtime::import_module(L, "main");
        check(lua_tointeger(L, -1) == round, "a reopened runtime starts with an empty module cache");
        lua_pop(L, 1);
        arky::runtime::close(L);
    }
    check(files.loads["main"] == 3 && lua_gettop(L) == 0, "the module was read once per runtime");
}

// Lua states which end while worker tasks spin and file reads are in flight.
void teardown() {
    project files;
    files.modules["busy"] = R"lua(
local task = require("tynex_task")
task.configure(2, 32)
for i = 1, 8 do
   scheduler.from_worker(task.spawn(function(_, context)
      while true do context.checkpoint() end
   end))
end
local read = __tynex_async_runtime.create(function(path) return __tynex_async_runtime.await(fs.read_file(path)) end)
for i = 1, 8 do read("slow://" .. i) end
)lua";
    auto settings = files.options();
    settings.file_reader = [](std::string_view, std::size_t, std::string& bytes, std::string&) {
        std::this_thread::sleep_for(2ms);
        bytes = "late";
        return true;
    };
    for (int round = 0; round < 20; ++round) {
        arky::state vm;
        lua_State* L = vm.get();
        arky::runtime::open(L, settings);
        arky::runtime::import_module(L, "busy");
        lua_pop(L, 1);
        arky::runtime::step(L);
        if (round % 2) arky::runtime::close(L);
    }
}

// What a finalizer met while its Lua state was closing.
struct {
    std::string handle;
    std::string import;
    bool balanced = false;
} closing;
int ask_while_closing(lua_State* L) {
    const int top = lua_gettop(L);
    try {
        const arky::runtime::deferred late(L);
        closing.handle = "created";
    } catch (const std::exception& failure) {
        closing.handle = failure.what();
    }
    closing.balanced = lua_gettop(L) == top;
    return 0;
}
int report_import(lua_State* L) {
    const char* text = lua_tostring(L, 1);
    closing.import = text ? text : "";
    return 0;
}

// A Lua state which the host owns itself (luaL_newstate, lua_close), as a game engine does. Nothing
// tells the runtime beforehand that the state is closing: it only sees its own finalizers run.
void host_owned_state() {
    project files;
    files.modules["hook"] = "function import_late() return __tynex_import('late') end";
    files.modules["late"] = "return 1";
    closing = {};
    lua_State* L = luaL_newstate();
    check(L != nullptr, "cannot create a Lua state");
    luaL_openlibs(L);
    lua_pushcfunction(L, ask_while_closing);
    lua_setglobal(L, "ask");
    lua_pushcfunction(L, report_import);
    lua_setglobal(L, "report");
    // Older than the runtime, so finalized after it: by then the runtime's host object is gone, and
    // calls into the runtime must fail instead of using it.
    run(L, R"lua(
oldest = newproxy(true)
getmetatable(oldest).__gc = function()
   ask()
   local imported, missing = pcall(import_late)
   report(tostring(imported) .. "/" .. tostring(missing))
end
)lua");
    arky::runtime::open(L, files.options());
    arky::runtime::import_module(L, "hook");
    lua_pop(L, 1);
    const arky::runtime::deferred pending(L);
    lua_setglobal(L, "pending");
    check(pending.alive() && lua_gettop(L) == 0, "a handle of a host-owned Lua state is alive");
    lua_close(L);
    check(!pending.alive() && !pending.resolve(1) && !pending.reject("late"), "a handle outlives a Lua state closed by its host");
    check(closing.handle == "the Arky runtime is closing" && closing.balanced,
          ("a closing Lua state refuses a new handle and keeps the stack: " + closing.handle).c_str());
    check(closing.import.rfind("false/", 0) == 0 &&
              closing.import.find("unprepared static import 'late': the Arky runtime is closing") != std::string::npos,
          ("a closing Lua state refuses an import: " + closing.import).c_str());
}
} // namespace

int main() {
    try {
        lifecycle();
        imports();
        workers();
        file_reads();
        timers();
        recursion();
        deferred_tasks();
        isolation();
        restart();
        teardown();
        host_owned_state();
        std::cout << "Host runtime: lifecycle, imports, workers, file reader, timers, recursion, deferred tasks, "
                     "isolation, restart, teardown and host-owned Lua states passed\n";
    } catch (const std::exception& failure) {
        std::cerr << "host runtime: " << failure.what() << '\n';
        return 1;
    }
}
