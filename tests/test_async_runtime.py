"""Scheduler contract tests against stock LuaJIT, independent of syntax lowering."""
import json
import os
from pathlib import Path
import subprocess
import textwrap
import unittest

from test_cli import LUAJIT, ROOT


class AsyncRuntimeTests(unittest.TestCase):
    def lua(self, source):
        runtime = json.dumps(str(ROOT / "runtime/async.lua").replace("\\", "/"))
        code = f"local scheduler = assert(loadfile({runtime}))()\n" + textwrap.dedent(source)
        environment = {**os.environ, "LUA_CPATH": str(LUAJIT.parent / "?.so") + ";" + str(LUAJIT.parent / "?.dll") + ";;"}
        environment.pop("LUA_INIT", None)
        result = subprocess.run([str(LUAJIT), "-"], input=code, env=environment, cwd=ROOT,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def io_contract(self, source):
        # A trusted injected adapter lets these tests control readiness exactly.
        # Native userdata validation and actual I/O have separate integration tests.
        return self.lua(r"""
            local requests=setmetatable({}, {__mode='k'})
            local sockets=setmetatable({}, {__mode='k'})
            local waits,cancels,sleeps,collected=0,0,0,0
            local latest
            local function request(value)
                local handle=newproxy(true)
                requests[handle]={ready=false,value=value}
                latest=handle
                return handle
            end
            local native={}
            function native._async_ready(handle) return assert(requests[handle],'foreign request').ready end
            function native._async_wait(handle)
                local state=assert(requests[handle],'foreign request')
                assert(state.ready,'wait blocked')
                waits=waits+1
                return state.value
            end
            function native._async_cancel(handle)
                local state=assert(requests[handle],'foreign request')
                cancels=cancels+1
                state.cancelled=true
                return true
            end
            function native._async_sleep(milliseconds)
                assert(milliseconds == 1)
                sleeps=sleeps+1
                for _,state in pairs(requests) do state.ready=true end
            end
            function native.read_file(path,max_bytes,timeout_ms)
                assert(path == 'data.bin' and max_bytes == 100 and timeout_ms == 10)
                return request('a\0b')
            end
            function native.connect(host,port,timeout_ms)
                assert(host == 'localhost' and port == 1234 and timeout_ms == 10)
                local socket=newproxy(true)
                sockets[socket]={closed=false}
                getmetatable(socket).__gc=function() collected=collected+1 end
                return request(socket)
            end
            function native.read(socket,max_bytes,timeout_ms)
                assert(sockets[socket] and not sockets[socket].closed)
                assert(max_bytes == 100 and timeout_ms == 10)
                return request(nil)
            end
            function native.write(socket,data,timeout_ms)
                assert(sockets[socket] and not sockets[socket].closed)
                assert(data == 'payload' and timeout_ms == 10)
                return request(#data)
            end
            function native.close(socket)
                local state=assert(sockets[socket],'foreign socket')
                if state.closed then return false end
                state.closed=true
                return true
            end
            local isolated=assert(loadfile('runtime/async.lua'))(
                function() error('no worker library') end, function() return native end)
        """ + textwrap.dedent(source))

    def test_io_adapter_capture_eof_pending_cancel_and_private_socket_receivers(self):
        self.io_contract("""
            local fs,net=isolated.fs,isolated.net
            local file=fs.read_file('data.bin',100,10)
            assert(isolated.step() and waits == 0)
            assert(file:cancel() and cancels == 1 and not file:cancel())
            local cancelled=requests[latest]
            assert(cancelled.cancelled and isolated.pending() == 0)
            -- Later native module table mutation cannot redirect captured methods.
            for name in pairs(native) do native[name]=function() error('mutated native method') end end
            local data=isolated.run(fs.read_file('data.bin',100,10))
            assert(data == 'a\\0b' and sleeps >= 1 and waits == 1)
            local connected=net.connect('localhost',1234,10)
            local socket=isolated.run(connected)
            assert(connected:result() == socket and getmetatable(socket) == 'Tynex Socket')
            assert(not pcall(function() socket.read=function() end end))
            assert(not pcall(function() fs.read_file=function() end end))
            assert(not pcall(function() net.connect=function() end end))
            assert(not pcall(socket.read,{},100,10))
            assert(not pcall(socket.write,{},'payload',10))
            assert(not pcall(socket.close,{}))
            local eof=socket:read(100,10)
            assert(select('#',isolated.run(eof)) == 1 and eof:result() == nil)
            assert(isolated.run(socket:write('payload',10)) == 7)
            assert(socket:close() and not socket:close())
            assert(not pcall(socket.read,socket,100,10))
            assert(waits == 4 and isolated.pending() == 0)
            assert(not isolated.step() and waits == 4)
        """)

    def test_io_waiter_cancellation_keeps_shared_request_and_socket_graphs_collect(self):
        self.io_contract("""
            local dependency=isolated.fs.read_file('data.bin',100,10)
            local wait=isolated.create(function() return isolated.await(dependency) end)
            local first,second=wait(),wait()
            assert(isolated.step() and first:status() == 'waiting' and second:status() == 'waiting')
            assert(first:cancel() and cancels == 0)
            assert(isolated.run(second) == 'a\\0b' and cancels == 0 and waits == 1)
            local weak=setmetatable({}, {__mode='v'})
            do
                local socket=isolated.run(isolated.net.connect('localhost',1234,10))
                weak[1]=socket
                collectgarbage() collectgarbage()
                assert(weak[1] and collected == 0)
                local holder=isolated.create(function() return socket end)()
                isolated.run(holder)
                weak[2]=holder
                -- An arbitrary user cycle must not root the native socket.
                rawset(socket,'cycle',holder)
            end
            latest=nil
            for i=1,4 do collectgarbage() end
            assert(weak[1] == nil and weak[2] == nil and collected == 1)
        """)

    def test_missing_io_module_does_not_affect_coroutines(self):
        self.lua("""
            local absent=function() error('not installed') end
            local isolated=assert(loadfile('runtime/async.lua'))(absent,absent)
            assert(isolated.run(isolated.create(function() return 42 end)()) == 42)
            local ok,err=pcall(isolated.fs.read_file,'data.bin')
            assert(not ok and err:find('I/O adapter is unavailable',1,true))
            ok,err=pcall(isolated.net.connect,'localhost',1234)
            assert(not ok and err:find('I/O adapter is unavailable',1,true))
        """)

    def test_functions_start_at_the_call_and_the_step_budget_bounds_resumptions(self):
        self.assertEqual(self.lua("""
            local calls = {}
            local gate, open = scheduler.deferred()
            local fn = scheduler.create(function(n)
                calls[#calls+1] = 'start' .. n
                scheduler.await(gate)
                calls[#calls+1] = 'end' .. n
                return n
            end)
            local a,b,c = fn(1),fn(2),fn(3)
            -- Each ran at its call, up to its first await.
            assert(table.concat(calls,',') == 'start1,start2,start3')
            assert(a:status() == 'waiting' and scheduler.pending() == 4)
            local ok, err = pcall(a.result,a)
            assert(not ok and err:find('still pending',1,true))
            -- Nothing is runnable until the gate opens; what waited is then queued, never run by the opener.
            assert(scheduler.step() and #calls == 3)
            assert(open() and a:status() == 'queued' and #calls == 3)
            -- A step resumes at most its budget, in the order the functions began to wait.
            assert(scheduler.step(1) and a:ready() and not b:ready())
            assert(scheduler.step(1) and b:ready() and not c:ready())
            assert(not scheduler.step(1) and c:ready() and scheduler.pending() == 0)
            assert(table.concat(calls,',') == 'start1,start2,start3,end1,end2,end3')
            -- A function which never waits is finished when its call returns, and costs no step.
            local done = scheduler.create(function(n) return n * 2 end)(21)
            assert(done:status() == 'succeeded' and done:result() == 42 and scheduler.pending() == 0)
            -- So is one which fails: the call returns the failed Task, it does not raise.
            local failed = scheduler.create(function() error('early', 0) end)()
            ok, err = pcall(failed.result, failed)
            assert(failed:status() == 'failed' and not ok and err == 'early')
            -- An async function called by another starts inside it, and awaiting what is finished does not wait.
            local order = {}
            local inner = scheduler.create(function(n) order[#order+1] = 'inner' .. n return n end)
            local outer = scheduler.create(function()
                order[#order+1] = 'outer'
                local first = inner(1)
                order[#order+1] = first:status()
                return scheduler.await(first) + scheduler.await(inner(2))
            end)()
            assert(outer:result() == 3 and table.concat(order,',') == 'outer,inner1,succeeded,inner2')
            print(a:result(),b:result(),c:result())
        """), "1\t2\t3\n")

    def test_traceback_keeps_the_origin_across_a_function_started_in_between(self):
        self.lua("""
            local origin = scheduler.create(function() error('origin failure', 0) end)()
            local helper = scheduler.create(function() return 1 end)
            local function rethrown(between)
                local outer = scheduler.create(function()
                    local ok, failure = pcall(scheduler.await, origin)
                    assert(not ok and failure == 'origin failure')
                    between()
                    error(failure, 0)
                end)()
                local ok, failure = pcall(scheduler.run, outer)
                assert(not ok and failure == 'origin failure')
                return scheduler.__traceback(failure)
            end
            -- The trace of a failure which is passed on names where it came from...
            assert(rethrown(function() end):find('awaited from:', 1, true))
            -- ...also when the function started another one, which runs inside it, before passing it on.
            assert(rethrown(function() assert(helper():result() == 1) end):find('awaited from:', 1, true))
        """)

    def test_deep_recursion_is_queued_instead_of_exhausting_the_c_stack(self):
        self.lua("""
            -- Every start is a coroutine resumed inside its caller. Past a fixed depth the call is
            -- queued and starts at a later step, so this neither overflows the C stack nor fails.
            local depth, deepest = 0, 0
            local count
            count = scheduler.create(function(n)
                depth = depth + 1
                if depth > deepest then deepest = depth end
                local result = n == 0 and 0 or 1 + scheduler.await(count(n - 1))
                depth = depth - 1
                return result
            end)
            local task = count(50000)
            assert(task:status() == 'waiting')
            assert(scheduler.run(task) == 50000 and scheduler.pending() == 0)
            -- The functions did start inside one another, up to the limit.
            local nested = 0
            local probe
            probe = scheduler.create(function(n)
                nested = nested + 1
                if n > 0 then probe(n - 1) end
            end)
            probe(1000)
            assert(nested == 200, nested)
            assert(not scheduler.step(1000) and nested == 1001)
        """)

    def test_lua_values_nil_slots_and_zero_returns_survive_nested_await(self):
        self.lua("""
            local marker, callback, userdata = {}, function() end, newproxy(true)
            local first = scheduler.create(function(...) return ... end)
            local second = scheduler.create(function(task) return scheduler.await(task) end)
            local task = second(second(first(marker,nil,callback,userdata,false,nil)))
            local function verify(...)
                assert(select('#',...) == 6)
                local a,b,c,d,e,f = ...
                assert(a == marker and b == nil and c == callback and d == userdata and e == false and f == nil)
            end
            verify(scheduler.run(task))
            verify(task:result())
            local empty = scheduler.create(function() end)
            assert(select('#',scheduler.run(second(empty()))) == 0)
        """)

    def test_error_objects_are_exact_and_tracebacks_keep_async_origin(self):
        self.lua("""
            local function verify(value)
                local origin = scheduler.create(function() error(value,0) end)
                local forward = scheduler.create(function(task) return scheduler.await(task) end)
                local task = forward(origin())
                local ok, failure = pcall(scheduler.run,task)
                assert(not ok and failure == value and task:status() == 'failed')
                local trace = scheduler.__traceback(failure)
                assert(type(trace) == 'string' and trace:find('stdin:',1,true))
                ok, failure = pcall(task.result, task)
                assert(not ok and failure == value)
            end
            verify(nil) verify(false) verify({}) verify('failure')
            local value = setmetatable({}, {__tostring=function() error('bad tostring') end})
            verify(value)
            local comparison = {__eq=function() error('error equality must never run') end}
            local first,second = setmetatable({},comparison),setmetatable({},comparison)
            local failed=scheduler.create(function() error(first,0) end)()
            assert(not pcall(scheduler.run,failed))
            assert(scheduler.__traceback(second) == nil)
            -- Await an already-failed dependency and recover its exact value.
            local caught=scheduler.create(function()
                local ok,failure=pcall(scheduler.await,failed)
                assert(not ok and rawequal(failure,first))
                error(second,0)
            end)()
            local ok,failure=pcall(scheduler.run,caught)
            assert(not ok and rawequal(failure,second))
        """)

    def test_shared_waiters_cancel_independently_and_never_resume_twice(self):
        self.lua("""
            local calls = 0
            local dependency, resolve = scheduler.deferred()
            local waiter = scheduler.create(function()
                local n = scheduler.await(dependency)
                calls = calls+1
                return n
            end)
            local a,b = waiter(),waiter()
            assert(scheduler.step(2) and a:status() == 'waiting' and b:status() == 'waiting')
            assert(a:cancel() and not a:cancel() and not dependency:ready())
            assert(resolve(42))
            assert(scheduler.run(b) == 42 and calls == 1 and dependency:status() == 'succeeded')
            assert(not scheduler.step() and not scheduler.step() and calls == 1)
            local ok,err=pcall(a.result,a)
            assert(not ok and err == 'async task cancelled' and scheduler.pending() == 0)
        """)

    def test_dependency_cycles_and_raw_yields_are_diagnosed(self):
        self.lua("""
            -- A function starts at its call: it waits at a gate until the Tasks it refers to exist.
            local gate, open = scheduler.deferred()
            local a,b
            a=scheduler.create(function() scheduler.await(gate) return scheduler.await(b) end)()
            b=scheduler.create(function() scheduler.await(gate) return scheduler.await(a) end)()
            open()
            local ok,err=pcall(scheduler.run,a)
            assert(not ok and err:find('dependency cycle',1,true))
            assert(a:ready() and b:ready() and scheduler.pending() == 0)
            local raw=scheduler.create(function() coroutine.yield('arbitrary') end)()
            ok,err=pcall(scheduler.run,raw)
            assert(not ok and err:find('must suspend using await',1,true))
            local ok,err=pcall(scheduler.await,a)
            assert(not ok and err:find('only valid inside',1,true))
            local self
            gate, open = scheduler.deferred()
            self=scheduler.create(function() scheduler.await(gate) return scheduler.await(self) end)()
            open()
            ok,err=pcall(scheduler.run,self)
            assert(not ok and err:find('dependency cycle',1,true))
            -- A Task cancelled while its function runs is told at its next await.
            local cancelled, seen
            gate, open = scheduler.deferred()
            cancelled=scheduler.create(function()
                scheduler.await(gate)
                cancelled:cancel()
                seen = {pcall(scheduler.await, scheduler.sleep(0))}
            end)()
            open()
            scheduler.step()
            assert(cancelled:status() == 'cancelled' and seen[1] == false and seen[2]:find('async task cancelled',1,true), seen[2])
        """)

    def test_pump_reentrancy_and_invalid_budgets_are_rejected(self):
        self.lua("""
            local task=scheduler.create(function()
                local ok,err=pcall(scheduler.step)
                assert(not ok and err:find('inside an async',1,true))
                ok,err=pcall(scheduler.run,{})
                assert(not ok and err:find('inside an async',1,true))
                return 10
            end)()
            assert(scheduler.run(task) == 10)
            for _,value in ipairs({0,-1,1.5,1000001,math.huge,0/0,'1',false}) do
                local ok=pcall(scheduler.step,value)
                assert(not ok)
            end
            assert(not pcall(scheduler.run,{}))
            assert(not pcall(scheduler.create,{}))
            assert(not pcall(function() task.custom = 1 end))
        """)

    def test_native_bridge_is_exact_and_captures_c_functions(self):
        self.lua("""
            local task=require('tynex_task')
            task.configure(1,8)
            local forged_called=false
            local forged={ready=function() forged_called=true return true end,wait=function() return 99 end}
            assert(not pcall(scheduler.from_worker,forged) and not forged_called)
            assert(not pcall(scheduler.from_worker,newproxy(true)))
            local worker=task.spawn(function(n) return {result=n*2} end,21)
            local managed=scheduler.from_worker(worker)
            assert(managed == scheduler.from_worker(worker))
            task._async_ready=function() error('mutated ready') end
            task._async_wait=function() error('mutated wait') end
            task._async_cancel=function() error('mutated cancel') end
            task._async_sleep=function() error('mutated sleep') end
            package.loaded.tynex_task={}
            local join=scheduler.create(function() return scheduler.await(managed).result end)
            assert(scheduler.run(join()) == 42 and managed:result().result == 42)
            assert(scheduler.pending() == 0)
            task.shutdown()
        """)

    def test_native_waiting_step_never_blocks_and_cancel_is_cooperative(self):
        self.lua("""
            local task=require('tynex_task')
            task.configure(1,8)
            local handle=task.spawn(function(_,control) while true do control.checkpoint() end end)
            local managed=scheduler.from_worker(handle)
            local waiter=scheduler.create(function() return scheduler.await(managed) end)()
            assert(scheduler.step() and not managed:ready() and waiter:status() == 'waiting')
            assert(waiter:cancel() and not managed:ready())
            assert(managed:cancel() and managed:status() == 'cancelled')
            assert(scheduler.pending() == 0)
            assert(not scheduler.step())
            task.shutdown()
        """)

    def test_native_errors_propagate_and_coroutine_only_needs_no_library(self):
        self.lua("""
            local task=require('tynex_task')
            local managed=scheduler.from_worker(task.spawn(function() error('WORKER_ORIGIN') end))
            local ok,err=pcall(scheduler.run,managed)
            assert(not ok and err:find('WORKER_ORIGIN',1,true) and managed:status() == 'failed')
            task.shutdown()
            local fresh=assert(loadfile('runtime/async.lua'))(function() error('missing') end)
            assert(fresh.run(fresh.create(function() return 7 end)()) == 7)
            ok,err=pcall(fresh.from_worker,{})
            assert(not ok and err:find('adapter is unavailable',1,true))
        """)

    def test_runtime_captures_mutable_lua_builtins(self):
        self.lua("""
            local original_pcall, original_error = pcall,error
            local done=scheduler.create(function() return 9 end)()
            pcall=nil error=nil pairs=nil next=nil unpack=nil select=nil type=nil
            coroutine.resume=nil coroutine.status=nil coroutine.running=nil
            assert(scheduler.run(done) == 9 and scheduler.pending() == 0)
            local ok=original_pcall(done.result,done)
            assert(ok)
        """)

    def test_completed_and_cancelled_cyclic_task_graphs_are_collected(self):
        self.lua("""
            local references=setmetatable({}, {__mode='v'})
            do
                local gate, open = scheduler.deferred()
                local self
                self=scheduler.create(function() scheduler.await(gate) return self end)()
                references[1]=self
                open()
                assert(scheduler.run(self) == self)
                self=nil
            end
            do
                local gate, open = scheduler.deferred()
                local task
                task=scheduler.create(function() scheduler.await(gate) return task end)()
                references[2]=task
                assert(task:cancel() and open())
                task=nil
            end
            assert(not scheduler.step())
            collectgarbage() collectgarbage()
            assert(references[1] == nil and references[2] == nil)
            local retained=scheduler.create(function() return 42 end)()
            scheduler.run(retained)
            collectgarbage() collectgarbage()
            assert(retained:result() == 42)
        """)

    def lua_without_native(self, source):
        # A host with no native task or I/O module: nothing can tell the time or wait.
        runtime = json.dumps(str(ROOT / "runtime/async.lua").replace("\\", "/"))
        code = (f"local scheduler = assert(loadfile({runtime}))(function() end, function() end)\n"
                + textwrap.dedent(source))
        result = subprocess.run([str(LUAJIT), "-"], input=code, cwd=ROOT, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_host_time_timers_complete_in_due_order_only_when_time_is_stepped(self):
        self.lua("""
            local order = {}
            local function waiter(name, milliseconds)
                return scheduler.create(function()
                    assert(select('#', scheduler.await(scheduler.sleep(milliseconds))) == 0)
                    order[#order+1] = name
                end)()
            end
            waiter('c', 300) waiter('a', 100) waiter('b', 200) waiter('a2', 100) waiter('next', 0)
            -- The functions have started and wait; no time passes and nothing completes without a step.
            assert(scheduler.pending() == 10 and #order == 0)
            assert(scheduler.step(64, 0) and table.concat(order, ',') == 'next')
            assert(scheduler.step(64, 50) and #order == 1)
            assert(scheduler.step(64, 50) and table.concat(order, ',') == 'next,a,a2')
            assert(scheduler.step(64, 0) and #order == 3)
            assert(not scheduler.step(64, 500) and table.concat(order, ',') == 'next,a,a2,b,c')
            assert(scheduler.pending() == 0)
            -- A timer made during a step is first looked at in the next one: sleep(0) is "the next frame".
            local frames = scheduler.create(function()
                for frame = 1, 3 do
                    scheduler.await(scheduler.sleep(0))
                    order[#order+1] = 'frame' .. frame
                end
            end)()
            for frame = 1, 3 do
                scheduler.step(64, 0)
                assert(order[#order] == 'frame' .. frame and frames:ready() == (frame == 3))
            end
        """)

    def test_timer_arguments_and_cancellation(self):
        self.lua("""
            for _, name in ipairs({'sleep', 'sleep_realtime'}) do
                for _, value in ipairs({-1, -0.001, math.huge, 0/0, '1', false, {}}) do
                    local ok, err = pcall(scheduler[name], value)
                    assert(not ok and err:find('scheduler.' .. name .. ' expects milliseconds', 1, true), err)
                end
                assert(not pcall(scheduler[name]))
            end
            for _, value in ipairs({-1, math.huge, 0/0, '1', false}) do
                local ok, err = pcall(scheduler.step, 64, value)
                assert(not ok and err:find('scheduler.step expects milliseconds', 1, true), err)
            end
            -- A rejected step leaves the scheduler usable.
            assert(not scheduler.step(64, 0))

            local timer = scheduler.sleep(10)
            local waiter = scheduler.create(function() return scheduler.await(timer) end)()
            assert(scheduler.step(64, 1) and waiter:status() == 'waiting')
            assert(timer:cancel() and not timer:cancel() and timer:status() == 'cancelled')
            assert(not scheduler.step(64, 0) and waiter:status() == 'failed')
            local ok, err = pcall(waiter.result, waiter)
            assert(not ok and err == 'async task cancelled')
            -- The cancelled timer is still in its heap; reaching its time must not complete it again.
            assert(not scheduler.step(64, 100) and timer:status() == 'cancelled' and scheduler.pending() == 0)

            local many = {}
            for index = 1, 200 do many[index] = scheduler.sleep((index * 37) % 101 / 10) end
            for index = 1, 200, 3 do many[index]:cancel() end
            local elapsed, finished = 0, 0
            while scheduler.step(64, 0.25) do elapsed = elapsed + 0.25 end
            for index = 1, 200 do
                local expected = index % 3 == 1 and 'cancelled' or 'succeeded'
                assert(many[index]:status() == expected)
                finished = finished + 1
            end
            assert(finished == 200 and elapsed <= 10.25)

            -- Timers cancelled long before they are due are not kept until then. They are made
            -- inside a step, so they go straight into the heap.
            collectgarbage() collectgarbage()
            local before = collectgarbage('count')
            local kept
            scheduler.create(function()
                scheduler.await(scheduler.sleep(0))
                kept = scheduler.sleep(3)
                for index = 1, 200000 do scheduler.sleep(1000000 + index):cancel() end
            end)()
            assert(scheduler.step(64, 0) and kept and not kept:ready())
            collectgarbage() collectgarbage()
            assert(collectgarbage('count') < before + 2048, collectgarbage('count') - before)
            -- The heap was rebuilt many times around a waiting timer: it still completes on time.
            assert(scheduler.step(64, 2.5) and not kept:ready())
            assert(not scheduler.step(64, 0.5) and kept:ready() and scheduler.pending() == 0)
        """)

    def test_timers_made_outside_a_step_count_from_the_next_step(self):
        self.lua("""
            -- Between steps host time stands where the last step left it. The elapsed time of the
            -- next step went by, in part or in full, before a timer made now existed: it counts
            -- from that step on.
            assert(not scheduler.step(64, 0))
            local outside = scheduler.sleep(100)
            assert(scheduler.step(64, 150) and not outside:ready())
            assert(scheduler.step(64, 60) and not outside:ready())
            assert(not scheduler.step(64, 40) and outside:ready())
            -- sleep(0) made outside a step completes at the next step.
            local soon = scheduler.sleep(0)
            assert(not scheduler.step(64, 5) and soon:ready())
            -- Made inside a step, a timer counts from that step's time, the start of the frame.
            local made
            local maker = scheduler.create(function()
                scheduler.await(scheduler.sleep(0))
                made = scheduler.sleep(100)
                scheduler.await(made)
            end)()
            assert(scheduler.step(64, 10) and made and not made:ready())
            assert(scheduler.step(64, 90) and not made:ready())
            assert(not scheduler.step(64, 10) and made:ready() and maker:ready())
            -- A held timer cancelled before the next step never completes, and many of them are not kept.
            local dropped = scheduler.sleep(1)
            assert(dropped:cancel())
            assert(not scheduler.step(64, 10) and dropped:status() == 'cancelled')
            collectgarbage() collectgarbage()
            local before = collectgarbage('count')
            local kept = scheduler.sleep(50)
            for index = 1, 200000 do scheduler.sleep(index):cancel() end
            collectgarbage() collectgarbage()
            assert(collectgarbage('count') < before + 2048, collectgarbage('count') - before)
            assert(scheduler.step(64, 0) and not kept:ready())
            assert(not scheduler.step(64, 50) and kept:ready() and scheduler.pending() == 0)
        """)

    def test_real_time_timers_ignore_host_time_and_host_time_follows_real_time_by_default(self):
        self.lua("""
            local clock = require('tynex_task')._async_now
            local function spin(milliseconds)
                local finish = clock() + milliseconds
                while clock() < finish do end
            end
            -- A paused game: host time stands still while real time passes.
            local paused, real = scheduler.sleep(10), scheduler.sleep_realtime(10)
            spin(30)
            assert(scheduler.step(64, 0) and not paused:ready() and real:ready())
            assert(not scheduler.step(64, 10) and paused:ready())
            -- A fast-forwarded game: host time jumps, real time does not.
            real = scheduler.sleep_realtime(30000)
            assert(scheduler.step(64, 1000000) and not real:ready() and real:cancel())
            -- Without elapsed time host time follows the clock, so the CLI's blocking run can wait.
            local started = clock()
            scheduler.run(scheduler.create(function()
                scheduler.await(scheduler.sleep(20))
                scheduler.await(scheduler.sleep_realtime(20))
            end)())
            local waited = clock() - started
            assert(waited >= 40 and waited < 5000, waited)
            assert(scheduler.pending() == 0)
        """)

    def test_timers_without_a_native_clock(self):
        self.lua_without_native("""
            local ok, err = pcall(scheduler.sleep_realtime, 1)
            assert(not ok and err:find('needs the clock', 1, true), err)
            -- Host time still works for a host which passes the elapsed time.
            local timer = scheduler.sleep(1)
            assert(scheduler.step(64) and not timer:ready())
            assert(scheduler.step(64, 0.5) and not scheduler.step(64, 0.5) and timer:ready())
            ok, err = pcall(scheduler.run, scheduler.sleep(1))
            assert(not ok and err:find('cannot wait for a timer', 1, true), err)
        """)

    def test_deferred_tasks_are_completed_from_outside_exactly_once(self):
        self.lua("""
            local task, resolve, reject = scheduler.deferred()
            assert(task:status() == 'waiting' and scheduler.pending() == 1)
            local got
            local waiter = scheduler.create(function() got = {n = select('#', scheduler.await(task)), scheduler.await(task)} end)()
            assert(scheduler.step() and got == nil and waiter:status() == 'waiting')
            assert(resolve('a', nil, 3) == true and resolve('again') == false and reject('late') == false)
            assert(task:status() == 'succeeded' and got == nil)
            assert(not scheduler.step() and got.n == 3 and got[1] == 'a' and got[2] == nil and got[3] == 3)

            -- reject keeps the error value, and a value given before anyone waits is kept.
            local failed, _, fail = scheduler.deferred()
            local failure = {code = 7}
            assert(fail(failure) and not fail('again'))
            local ok, err = pcall(failed.result, failed)
            assert(not ok and err == failure)
            local early, settle_early = scheduler.deferred()
            settle_early(42)
            assert(scheduler.run(scheduler.create(function() return scheduler.await(early) end)()) == 42)

            -- The consumer may cancel; a late resolve is then refused.
            local cancelled, resolve_cancelled = scheduler.deferred()
            assert(cancelled:cancel() and resolve_cancelled(1) == false and cancelled:status() == 'cancelled')

            -- Completing from inside an async function resumes the waiter in a later resume.
            local inner, resolve_inner = scheduler.deferred()
            local order = {}
            scheduler.create(function() order[#order+1] = 'wait' local value = scheduler.await(inner) order[#order+1] = value end)()
            scheduler.create(function() resolve_inner('value') order[#order+1] = 'resolved' end)()
            assert(not scheduler.step() and table.concat(order, ',') == 'wait,resolved,value')

            -- A deferred Task nobody completes is a deadlock for the blocking run, not a hang.
            local never = scheduler.deferred()
            ok, err = pcall(scheduler.run, never)
            assert(not ok and err:find('deadlock', 1, true), err)
            assert(scheduler.pending() == 1)
        """)

    def test_all_keeps_the_order_given_and_fails_with_the_first_failure(self):
        self.lua("""
            local a, resolve_a = scheduler.deferred()
            local b, resolve_b = scheduler.deferred()
            local c, resolve_c = scheduler.deferred()
            -- A Task may be listed twice; values keep the order of the list, not of completion.
            local all = scheduler.all({a, b, a, c})
            assert(all:status() == 'waiting' and scheduler.pending() == 4)
            resolve_c('c', 'only the first value is kept')
            resolve_a('a')
            assert(all:status() == 'waiting')
            resolve_b()
            assert(all:status() == 'succeeded' and select('#', all:result()) == 1 and scheduler.pending() == 0)
            local values = all:result()
            assert(values[1] == 'a' and values[2] == nil and values[3] == 'a' and values[4] == 'c')

            -- Nothing to wait for: complete at once.
            local empty = scheduler.all({})
            assert(empty:status() == 'succeeded' and next(empty:result()) == nil)
            local done = scheduler.all({a, c})
            assert(done:status() == 'succeeded' and done:result()[2] == 'c')

            -- The first failure fails the whole, with the same error value; the others are left alone.
            local slow, resolve_slow = scheduler.deferred()
            local broken, _, reject_broken = scheduler.deferred()
            local late, _, reject_late = scheduler.deferred()
            local failing = scheduler.all({slow, broken, late})
            local failure = {code = 7}
            assert(reject_broken(failure) and failing:status() == 'failed')
            local ok, err = pcall(failing.result, failing)
            assert(not ok and err == failure)
            assert(slow:status() == 'waiting' and late:status() == 'waiting')
            assert(reject_late('second') and resolve_slow(1) and failing:status() == 'failed')
            ok, err = pcall(failing.result, failing)
            assert(not ok and err == failure)
            -- A Task which had already failed fails the whole at once.
            local already = scheduler.all({a, late, broken})
            ok, err = pcall(already.result, already)
            assert(already:status() == 'failed' and not ok and err == 'second')

            -- A cancelled Task counts as a failure. Cancelling the whole leaves its Tasks running.
            local dropped = scheduler.deferred()
            local waiting = scheduler.all({dropped})
            assert(dropped:cancel() and waiting:status() == 'failed')
            ok, err = pcall(waiting.result, waiting)
            assert(not ok and err == 'async task cancelled')
            local kept, resolve_kept = scheduler.deferred()
            local abandoned = scheduler.all({kept})
            assert(abandoned:cancel() and kept:status() == 'waiting' and resolve_kept('still') and kept:result() == 'still')
            assert(abandoned:status() == 'cancelled')

            -- An async function awaits the whole like any Task, and sees its failure.
            local first, resolve_first = scheduler.deferred()
            local second, resolve_second, reject_second = scheduler.deferred()
            local sum = scheduler.create(function()
                local values = scheduler.await(scheduler.all({first, second}))
                return values[1] + values[2]
            end)()
            assert(scheduler.step() and sum:status() == 'waiting')
            resolve_second(2) resolve_first(40)
            assert(not scheduler.step() and sum:result() == 42)
            local third, _, reject_third = scheduler.deferred()
            local caught = scheduler.create(function()
                local ok, err = pcall(scheduler.await, scheduler.all({third}))
                return ok, err
            end)()
            scheduler.step()
            reject_third(failure)
            scheduler.step()
            ok, err = caught:result()
            assert(ok == false and err == failure)

            for _, value in ipairs({false, 1, 'tasks', {1}, {a, 'b'}, {{}}}) do
                local before = scheduler.pending()
                ok, err = pcall(scheduler.all, value)
                assert(not ok and err:find('scheduler.all expects an array of Tasks', 1, true), err)
                assert(scheduler.pending() == before)
            end
            assert(not pcall(scheduler.all))
        """)

    def test_race_settles_as_the_first_task_does(self):
        self.lua("""
            local a, resolve_a = scheduler.deferred()
            local b, resolve_b = scheduler.deferred()
            local race = scheduler.race({a, b, a})
            assert(race:status() == 'waiting' and scheduler.pending() == 3)
            -- All the winner's values, nil included.
            assert(resolve_b('b', nil, 3) and race:status() == 'succeeded')
            local function check(...)
                assert(select('#', ...) == 3)
                local first, second, third = ...
                assert(first == 'b' and second == nil and third == 3)
            end
            check(race:result())
            -- The loser keeps running and changes nothing when it settles.
            assert(a:status() == 'waiting' and resolve_a('a') and a:result() == 'a')
            check(race:result())
            assert(scheduler.pending() == 0)

            -- A failure which comes first fails the race; a Task which is already settled wins at once.
            local slow = scheduler.deferred()
            local broken, _, reject_broken = scheduler.deferred()
            local failing = scheduler.race({slow, broken})
            local failure = {code = 7}
            reject_broken(failure)
            local ok, err = pcall(failing.result, failing)
            assert(failing:status() == 'failed' and not ok and err == failure and slow:status() == 'waiting')
            assert(scheduler.race({slow, a, b}):result() == 'a')
            ok, err = pcall(function() return scheduler.race({broken, a}):result() end)
            assert(not ok and err == failure)

            -- A timeout: the timer wins with no value, and the Task it guarded is still waiting.
            local answer, resolve_answer = scheduler.deferred()
            local guarded = scheduler.race({answer, scheduler.sleep(100)})
            -- Made outside a step, the timer starts counting at the next step.
            assert(scheduler.step(64, 0) and guarded:status() == 'waiting')
            assert(scheduler.step(64, 50) and guarded:status() == 'waiting')
            scheduler.step(64, 50)
            assert(guarded:status() == 'succeeded' and select('#', guarded:result()) == 0)
            assert(answer:status() == 'waiting' and resolve_answer('late') and select('#', guarded:result()) == 0)

            -- Combined Tasks combine, and cancelling one leaves what it waited for.
            local x, resolve_x = scheduler.deferred()
            local y, resolve_y = scheduler.deferred()
            local z, resolve_z = scheduler.deferred()
            local nested = scheduler.race({scheduler.all({x, y}), z})
            resolve_x(1)
            assert(nested:status() == 'waiting')
            resolve_y(2)
            local values = nested:result()
            assert(values[1] == 1 and values[2] == 2 and z:status() == 'waiting')
            local abandoned = scheduler.race({z})
            assert(abandoned:cancel() and z:status() == 'waiting' and resolve_z(3) and abandoned:status() == 'cancelled')

            -- A race nobody runs in would never end.
            ok, err = pcall(scheduler.race, {})
            assert(not ok and err:find('at least one Task', 1, true), err)
            for _, value in ipairs({false, 1, {1}, {a, false}}) do
                local before = scheduler.pending()
                ok, err = pcall(scheduler.race, value)
                assert(not ok and err:find('scheduler.race expects an array of Tasks', 1, true), err)
                assert(scheduler.pending() == before)
            end
            assert(scheduler.pending() == 1)   -- `slow`, which nobody completes
        """)

    def test_combined_tasks_do_not_pile_up_on_a_long_lived_task_and_cycles_are_found(self):
        self.lua("""
            -- A Task which outlives many races (a "closed" signal, say) must not keep them all.
            local forever = scheduler.deferred()
            collectgarbage() collectgarbage()
            local before = collectgarbage('count')
            for index = 1, 100000 do
                local quick, finish = scheduler.deferred()
                local race = scheduler.race({forever, quick})
                local all = scheduler.all({forever, quick})
                finish(index)
                assert(race:result() == index and all:status() == 'waiting' and all:cancel())
            end
            collectgarbage() collectgarbage()
            assert(collectgarbage('count') < before + 1024, collectgarbage('count') - before)
            assert(scheduler.pending() == 1)

            -- Waiting for oneself through combined Tasks is an error at the await, not a silent deadlock.
            local gate, open = scheduler.deferred()
            local own
            own = scheduler.create(function()
                scheduler.await(gate)
                return scheduler.await(scheduler.race({scheduler.all({own})}))
            end)()
            scheduler.step()
            open()
            scheduler.step()
            local ok, err = pcall(own.result, own)
            assert(own:status() == 'failed' and not ok and err:find('dependency cycle', 1, true), err)
        """)


if __name__ == '__main__':
    unittest.main()
