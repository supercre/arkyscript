-- One scheduler belongs to one Lua VM and must be pumped on its owner thread.
-- Hosts may pass trusted task and I/O module loaders when loading this chunk.
local worker_loader, io_loader = ...
local raw_require, protected_call, raise = require, pcall, error
local lua_type, set_meta, lua_pairs, lua_next, raw_equal = type, setmetatable, pairs, next, rawequal
local unpack_values, count_values = unpack, select
local coroutine_create, coroutine_resume = coroutine.create, coroutine.resume
local coroutine_yield, coroutine_status, coroutine_running = coroutine.yield, coroutine.status, coroutine.running
local traceback, stringify = debug.traceback, tostring
local floor, huge = math.floor, math.huge
local function pack(...) return {n=count_values("#", ...), ...} end
local function text(value)
   local ok, result = protected_call(stringify, value)
   return ok and result or "<error object>"
end

-- Capture the native adapter before user modules can replace require, loaders,
-- package.loaded, or the native module table. A missing worker library is fine
-- for programs which use only coroutine tasks.
local function capture_adapter(loader, name)
   local ok, native = protected_call(loader or function() return raw_require(name) end)
   if not ok or lua_type(native) ~= "table" then return end
   local adapter = {ready=native._async_ready, wait=native._async_wait,
      cancel=native._async_cancel, sleep=native._async_sleep}
   for _, fn in lua_pairs(adapter) do
      if lua_type(fn) ~= "function" then return end
   end
   if not adapter.ready or not adapter.wait or not adapter.cancel or not adapter.sleep then return end
   -- The monotonic clock is optional: a module built before timers existed has none.
   if lua_type(native._async_now) == "function" then adapter.now = native._async_now end
   return adapter, native
end
local worker_adapter = capture_adapter(worker_loader, "tynex_task")
local io_adapter, io_native = capture_adapter(io_loader, "tynex_io")
local io_read_file, io_connect, io_read, io_write, io_close
if io_adapter then
   io_read_file, io_connect, io_read, io_write, io_close = io_native.read_file, io_native.connect,
      io_native.read, io_native.write, io_native.close
   if lua_type(io_read_file) ~= "function" or lua_type(io_connect) ~= "function"
      or lua_type(io_read) ~= "function" or lua_type(io_write) ~= "function" or lua_type(io_close) ~= "function" then
      io_adapter = nil
   end
end
io_native = nil
local sleep = worker_adapter and worker_adapter.sleep or io_adapter and io_adapter.sleep
local now = worker_adapter and worker_adapter.now or io_adapter and io_adapter.now

-- Lua 5.1 weak-key tables are not ephemerons: storing a state which contains
-- its own Task result would otherwise keep that entire result graph alive.
local states = set_meta({}, {__mode="kv"})
local worker_tasks = set_meta({}, {__mode="kv"})
local requests = {}
local queue, first, last = {}, 1, 0
local active, current, pumping = 0, nil, false
local yield_marker = {}
local last_failure
local methods, scheduler = {}, {}
local function readonly_task()
   raise("Task objects are read-only", 2)
end
local function record(task)
   local state = states[task]
   if not state then raise("expected a managed Task", 3) end
   return state
end
local function finished(state)
   return state.status == "succeeded" or state.status == "failed" or state.status == "cancelled"
end
local function enqueue(state)
   state.status = "queued"
   last = last + 1
   queue[last] = state
end
local function new_task(state)
   state.methods = methods
   -- The Task owns its state through this closure; only active work is rooted
   -- by the scheduler. The registry merely validates a Task's identity.
   local task = set_meta({}, {__index=function(_, key) return state.methods[key] end,
      __metatable="Tynex Task", __newindex=readonly_task})
   state.waiters = {}
   states[task] = state
   active = active + 1
   return task
end
local function detach_waiter(state)
   local dependency = state.waiting_on
   if dependency then
      local retained = {}
      for i=1,#dependency.waiters do
         local waiter = dependency.waiters[i]
         if waiter ~= state then retained[#retained+1] = waiter end
      end
      dependency.waiters = retained
      state.waiting_on = nil
   end
end
-- A combined Task (scheduler.all, scheduler.race) which is settled no longer waits
-- for its other inputs: a Task which outlives many of them must not collect them.
local function release_inputs(state)
   local inputs = state.inputs
   state.inputs, state.notify = nil, nil
   for i=1,#inputs do
      local dependency = inputs[i]
      if not finished(dependency) then
         local retained = {}
         for j=1,#dependency.waiters do
            local waiter = dependency.waiters[j]
            if waiter ~= state then retained[#retained+1] = waiter end
         end
         dependency.waiters = retained
      end
   end
end
local function settle(state, status, values, failure, trace)
   if finished(state) then return false end
   detach_waiter(state)
   state.status, state.values, state.failure, state.trace = status, values, failure, trace
   state.thread, state.arguments, state.request, state.adapter, state.transform = nil, nil, nil, nil, nil
   requests[state] = nil
   if state.clock then
      state.clock.waiting = state.clock.waiting - 1
      state.clock = nil
   end
   active = active - 1
   if state.inputs then release_inputs(state) end
   -- Waiters hear of it in the order they began to wait: async functions are
   -- queued, combined Tasks are told at once and may settle in turn.
   local waiters = state.waiters
   state.waiters = {}
   for i=1,#waiters do
      local waiter = waiters[i]
      if waiter.notify then
         waiter.notify(state)
      elseif waiter.status == "waiting" and waiter.waiting_on == state then
         waiter.waiting_on = nil
         enqueue(waiter)
      end
   end
   return true
end
local function result(state)
   if state.status == "succeeded" then return unpack_values(state.values, 1, state.values.n) end
   if state.status == "failed" or state.status == "cancelled" then
      last_failure = state
      raise(state.failure, 0)
   end
   raise("async task is still pending; use await or scheduler.run", 3)
end
function methods:ready() return finished(record(self)) end
function methods:status() return record(self).status end
function methods:result() return result(record(self)) end
function methods:cancel()
   local state = record(self)
   if finished(state) then return false end
   if state.request then
      -- Cancellation belongs to this explicit wrapper, never to an awaited
      -- dependency merely because one of its waiters was cancelled.
      local ok, failure = protected_call(state.adapter.cancel, state.request)
      if not ok then
         settle(state, "failed", nil, failure, traceback(text(failure), 2))
         return false
      end
   end
   return settle(state, "cancelled", nil, "async task cancelled", traceback("async task cancelled", 2))
end
local resume_one
-- An async function runs at its call, up to its first await, as a plain function
-- would: the caller sees what it did before it waited. Every such start is a
-- coroutine resumed inside the caller, so calls nested deeper than this are
-- queued instead and start at the next step: recursion as deep as memory allows
-- must not exhaust the C stack.
local nesting, nesting_limit = 0, 200
local function start(state)
   if nesting >= nesting_limit then return enqueue(state) end
   state.status = "queued"
   nesting = nesting + 1
   resume_one(state)
   nesting = nesting - 1
end
function scheduler.create(fn)
   if lua_type(fn) ~= "function" then raise("async expects a function", 2) end
   return function(...)
      local state = {status="created", thread=coroutine_create(fn), arguments=pack(...)}
      local task = new_task(state)
      start(state)
      return task
   end
end
-- Whether state waits for target, directly or through the Tasks it waits for.
local function waits_for(state, target, seen)
   while state do
      if state == target then return true end
      local inputs = state.inputs
      if inputs then
         seen = seen or {}
         if seen[state] then return false end
         seen[state] = true
         for i=1,#inputs do
            if waits_for(inputs[i], target, seen) then return true end
         end
         return false
      end
      state = state.waiting_on
   end
   return false
end
function scheduler.await(task)
   if not current then raise("await is only valid inside a managed async function", 2) end
   -- Cancelled while it ran, by itself or by a function it called: it stops at its next await.
   if finished(current) then raise("async task cancelled", 2) end
   if coroutine_running() ~= current.thread then
      raise("await is only valid inside a managed async function", 2)
   end
   local dependency = record(task)
   if finished(dependency) then return result(dependency) end
   if waits_for(dependency, current) then raise("async await dependency cycle", 2) end
   coroutine_yield(yield_marker, dependency)
   return result(dependency)
end
-- Timers, in milliseconds like every other time of this API. Host time advances
-- by what the host passes to step: a game's frame time, so pausing or scaling
-- the game applies to it. Real time follows the monotonic clock of a native
-- module. A host which passes no elapsed time (the CLI, for one) lets host time
-- follow real time.
-- Each clock keeps a binary heap of waiting timers: earliest first, then in the
-- order they were created. A cancelled timer stays in the heap until it is due.
-- Host time moves only when a step begins, so it stands at the start of the
-- current frame. A host-time timer made inside a step counts from there, as
-- frame-based games do. One made outside a step (an input handler, a module
-- being loaded, between frames) is held until the next step and counts from
-- that step's time: the elapsed time the host passes to it went by, in part or
-- in full, before the timer existed.
local host_clock = {time=0, heap={}, size=0, waiting=0, held={}, held_size=0}
local real_clock = {time=0, heap={}, size=0, waiting=0}
local timer_order = 0
local last_real = now and now()
local function earlier(a, b)
   return a.due < b.due or (a.due == b.due and a.order < b.order)
end
local function push_timer(clock, state)
   local heap, index = clock.heap, clock.size + 1
   clock.size = index
   while index > 1 do
      local parent = floor(index / 2)
      if not earlier(state, heap[parent]) then break end
      heap[index] = heap[parent]
      index = parent
   end
   heap[index] = state
end
local function pop_timer(clock)
   local heap, size = clock.heap, clock.size
   local top, moved = heap[1], heap[size]
   heap[size] = nil
   size = size - 1
   clock.size = size
   if size > 0 then
      local index = 1
      while true do
         local child = index * 2
         if child > size then break end
         if child < size and earlier(heap[child + 1], heap[child]) then child = child + 1 end
         if not earlier(heap[child], moved) then break end
         heap[index] = heap[child]
         index = child
      end
      heap[index] = moved
   end
   return top
end
local function fire_timers(clock)
   local heap = clock.heap
   while clock.size > 0 and heap[1].due <= clock.time do
      settle(pop_timer(clock), "succeeded", {n=0})
   end
end
local function duration(milliseconds, name, level)
   -- The comparison is false for NaN too.
   if lua_type(milliseconds) ~= "number" or not (milliseconds >= 0) or milliseconds == huge then
      raise(name .. " expects milliseconds: a number, zero or more", level)
   end
end
-- Cancelled timers are dropped once they outnumber the waiting ones: a timeout
-- which is cancelled each time would otherwise stay until its distant due time.
local function compact(clock)
   local heap, size = clock.heap, clock.size
   clock.heap, clock.size = {}, 0
   for index = 1, size do
      if not finished(heap[index]) then push_timer(clock, heap[index]) end
   end
end
-- Holds a timer until the next step. Cancelled ones are dropped once they
-- outnumber the waiting timers, as in compact.
local function hold(clock, state)
   local held, size = clock.held, clock.held_size
   if size > 64 and size > 2 * clock.waiting then
      local kept, count = {}, 0
      for index = 1, size do
         if not finished(held[index]) then
            count = count + 1
            kept[count] = held[index]
         end
      end
      held, size = kept, count
      clock.held = held
   end
   size = size + 1
   held[size] = state
   clock.held_size = size
end
-- At the start of a step, after the clock moved: held timers count from now.
local function release_held(clock)
   local held, size = clock.held, clock.held_size
   if size == 0 then return end
   clock.held, clock.held_size = {}, 0
   for index = 1, size do
      local state = held[index]
      if not finished(state) then
         state.due = clock.time + state.duration
         push_timer(clock, state)
      end
   end
end
local function new_timer(clock, due, length)
   if clock.size > 64 and clock.size > 2 * clock.waiting then compact(clock) end
   timer_order = timer_order + 1
   local state = {status="waiting", clock=clock, due=due, duration=length, order=timer_order}
   local task = new_task(state)
   clock.waiting = clock.waiting + 1
   if due then push_timer(clock, state) else hold(clock, state) end
   return task
end
-- The Task completes, with no value, once that much host time has passed.
-- sleep(0) completes at the next step: "the next frame".
function scheduler.sleep(milliseconds)
   duration(milliseconds, "scheduler.sleep", 3)
   if pumping then return new_timer(host_clock, host_clock.time + milliseconds) end
   return new_timer(host_clock, nil, milliseconds)
end
function scheduler.sleep_realtime(milliseconds)
   duration(milliseconds, "scheduler.sleep_realtime", 3)
   if not now then raise("scheduler.sleep_realtime needs the clock of the native task or I/O module", 2) end
   return new_timer(real_clock, now() + milliseconds)
end
-- Moves the clocks to the start of a step and completes the timers which are due.
local function advance(elapsed)
   local real = now and now()
   if elapsed then
      host_clock.time = host_clock.time + elapsed
   elseif real then
      host_clock.time = host_clock.time + (real - last_real)
   end
   release_held(host_clock)
   if real then
      last_real = real
      real_clock.time = real
      fire_timers(real_clock)
   end
   fire_timers(host_clock)
end

-- A Task which is completed from outside the scheduler: by a callback, an
-- event, the host. resolve(...) completes it with those values and reject(e)
-- fails it; each returns true only for the call which completed the Task.
-- Call them on the thread which owns this Lua state.
function scheduler.deferred()
   local state = {status="waiting"}
   local task = new_task(state)
   local function resolve(...)
      return settle(state, "succeeded", pack(...))
   end
   local function reject(failure)
      return settle(state, "failed", nil, failure, traceback(text(failure), 2))
   end
   return task, resolve, reject
end

-- Several Tasks as one. scheduler.all succeeds once every Task has, with an
-- array of each one's first value in the order given, and fails as soon as one
-- of them fails or is cancelled. scheduler.race settles as the first Task to
-- settle does, with all its values. Neither cancels the Tasks it was given, and
-- cancelling the combined Task leaves them running.
local function inputs_of(tasks, name)
   if lua_type(tasks) ~= "table" then raise(name .. " expects an array of Tasks", 3) end
   local inputs = {}
   for index=1,#tasks do
      local state = states[tasks[index]]
      if not state then raise(name .. " expects an array of Tasks", 3) end
      inputs[index] = state
   end
   return inputs
end
-- Registers a combined Task with the pending Tasks it waits for.
local function watch(state, watched, notify)
   state.inputs, state.notify = watched, notify
   for i=1,#watched do
      local waiters = watched[i].waiters
      waiters[#waiters+1] = state
   end
end
function scheduler.all(tasks)
   local inputs = inputs_of(tasks, "scheduler.all")
   local state = {status="waiting"}
   local task = new_task(state)
   local results, remaining = {}, #inputs
   -- A Task may be listed more than once: positions[its state] are its places.
   local positions, watched = {}, {}
   for index=1,#inputs do
      local dependency = inputs[index]
      if not finished(dependency) then
         local places = positions[dependency]
         if not places then
            places = {}
            positions[dependency] = places
            watched[#watched+1] = dependency
         end
         places[#places+1] = index
      elseif dependency.status == "succeeded" then
         results[index] = dependency.values[1]
         remaining = remaining - 1
      else
         settle(state, "failed", nil, dependency.failure, dependency.trace)
         return task
      end
   end
   if remaining == 0 then
      settle(state, "succeeded", {n=1, results})
      return task
   end
   watch(state, watched, function(dependency)
      if dependency.status ~= "succeeded" then
         settle(state, "failed", nil, dependency.failure, dependency.trace)
         return
      end
      local places = positions[dependency]
      for i=1,#places do results[places[i]] = dependency.values[1] end
      remaining = remaining - #places
      if remaining == 0 then settle(state, "succeeded", {n=1, results}) end
   end)
   return task
end
function scheduler.race(tasks)
   local inputs = inputs_of(tasks, "scheduler.race")
   -- A race nobody runs in would never end.
   if #inputs == 0 then raise("scheduler.race expects at least one Task", 2) end
   local state = {status="waiting"}
   local task = new_task(state)
   local function finish(dependency)
      if dependency.status == "succeeded" then settle(state, "succeeded", dependency.values)
      else settle(state, "failed", nil, dependency.failure, dependency.trace) end
   end
   for index=1,#inputs do
      if finished(inputs[index]) then
         finish(inputs[index])
         return task
      end
   end
   -- A Task listed twice is told twice; the second time changes nothing.
   watch(state, inputs, finish)
   return task
end

local function wrap_request(handle, adapter, transform)
   -- The C adapter checks exact userdata storage, metatable, and private tag.
   -- Never invoke arbitrary ready/wait/cancel methods supplied by the caller.
   local ok, ready = protected_call(adapter.ready, handle)
   if not ok then raise(ready, 2) end
   local state = {status="waiting", request=handle, adapter=adapter, transform=transform}
   local task = new_task(state)
   requests[state] = true
   return task
end
function scheduler.from_worker(handle)
   if not worker_adapter then raise("native task adapter is unavailable; rebuild tynex_task", 2) end
   -- Validate cached handles too: host code may explicitly release userdata.
   local ok, ready = protected_call(worker_adapter.ready, handle)
   if not ok then raise(ready, 2) end
   local previous = worker_tasks[handle]
   if previous then return previous end
   local task = wrap_request(handle, worker_adapter)
   worker_tasks[handle] = task
   return task
end
local socket_states = set_meta({}, {__mode="kv"})
local socket_methods = {}
local function socket_record(socket)
   local state = socket_states[socket]
   if not state then raise("expected a managed Socket", 3) end
   return state
end
local function wrap_socket(handle)
   if lua_type(handle) ~= "userdata" then raise("native connect returned an invalid socket", 2) end
   local state = {handle=handle, methods=socket_methods}
   local socket = set_meta({}, {__index=function(_, key) return state.methods[key] end,
      __metatable="Tynex Socket", __newindex=function() raise("Socket objects are read-only", 2) end})
   socket_states[socket] = state
   return socket
end
function socket_methods:read(max_bytes, timeout_ms)
   local handle = socket_record(self).handle
   return wrap_request(io_read(handle, max_bytes, timeout_ms), io_adapter)
end
function socket_methods:write(data, timeout_ms)
   local handle = socket_record(self).handle
   return wrap_request(io_write(handle, data, timeout_ms), io_adapter)
end
function socket_methods:close()
   return io_close(socket_record(self).handle)
end
local fs, net = {}, {}
local function require_io()
   if not io_adapter then raise("native I/O adapter is unavailable; rebuild tynex_io", 3) end
end
function fs.read_file(path, max_bytes, timeout_ms)
   require_io()
   return wrap_request(io_read_file(path, max_bytes, timeout_ms), io_adapter)
end
function net.connect(host, port, timeout_ms)
   require_io()
   return wrap_request(io_connect(host, port, timeout_ms), io_adapter, wrap_socket)
end
local function readonly_api(api, name)
   return set_meta({}, {__index=api, __metatable="Tynex " .. name,
      __newindex=function() raise(name .. " methods are read-only", 2) end})
end
scheduler.fs, scheduler.net = readonly_api(fs, "fs"), readonly_api(net, "net")
local function poll_requests()
   for state in lua_pairs(requests) do
      local ok, ready = protected_call(state.adapter.ready, state.request)
      if not ok then
         settle(state, "failed", nil, ready, traceback(text(ready), 2))
      elseif ready then
         local values = pack(protected_call(state.adapter.wait, state.request))
         if values[1] and state.transform then
            values = pack(protected_call(state.transform, unpack_values(values, 2, values.n)))
         end
         if values[1] then
            local returns = {n=values.n-1}
            for i=2,values.n do returns[i-1] = values[i] end
            settle(state, "succeeded", returns)
         else
            settle(state, "failed", nil, values[2], traceback(text(values[2]), 2))
         end
      end
   end
end
function resume_one(state)
   if state.status ~= "queued" then return end
   state.status = "running"
   -- The function may be starting inside one which is running. That one is
   -- current again when this returns, with the failure it last looked at.
   local caller, caller_failure = current, last_failure
   current = state
   last_failure = nil
   local arguments = state.arguments
   state.arguments = nil
   local values
   if arguments then values = pack(coroutine_resume(state.thread, unpack_values(arguments, 1, arguments.n)))
   else values = pack(coroutine_resume(state.thread)) end
   current = caller
   if finished(state) then
      -- Cancelled while it ran: what its coroutine did afterwards does not count.
   elseif not values[1] then
      local trace = traceback(state.thread, text(values[2]), 0)
      if last_failure and raw_equal(last_failure.failure, values[2]) and last_failure.trace then
         trace = last_failure.trace .. "\nawaited from:\n" .. trace
      end
      last_failure = nil
      settle(state, "failed", nil, values[2], trace)
   elseif coroutine_status(state.thread) == "dead" then
      local returns = {n=values.n-1}
      for i=2,values.n do returns[i-1] = values[i] end
      settle(state, "succeeded", returns)
   elseif values[2] == yield_marker and values[3] then
      local dependency = values[3]
      state.status, state.waiting_on = "waiting", dependency
      dependency.waiters[#dependency.waiters+1] = state
   else
      local failure = "async functions must suspend using await, not coroutine.yield"
      settle(state, "failed", nil, failure, traceback(state.thread, failure, 0))
   end
   if caller then last_failure = caller_failure end
end
local function step(max_resumes, elapsed)
   if pumping or current then raise("scheduler.step cannot run inside an async function or another pump", 2) end
   if max_resumes == nil then max_resumes = 64 end
   if lua_type(max_resumes) ~= "number" or max_resumes < 1 or max_resumes > 1000000 or max_resumes ~= floor(max_resumes) then
      raise("scheduler.step expects an integer budget from 1 to 1000000", 2)
   end
   if elapsed ~= nil then duration(elapsed, "scheduler.step", 3) end
   pumping = true
   advance(elapsed)
   poll_requests()
   local resumed = 0
   while first <= last and resumed < max_resumes do
      local state = queue[first]
      queue[first] = nil
      first = first + 1
      if state.status == "queued" then
         resumed = resumed + 1
         resume_one(state)
      end
   end
   if first > last then queue, first, last = {}, 1, 0 end
   pumping = false
   return active > 0
end
scheduler.step = step
function scheduler.pending() return active end
function scheduler.run(task)
   if pumping or current then raise("scheduler.run cannot run inside an async function or another pump", 2) end
   local state = record(task)
   while not finished(state) do
      step()
      if not finished(state) and first > last then
         if lua_next(requests) then sleep(1)
         elseif host_clock.waiting > 0 or real_clock.waiting > 0 then
            -- Only a native module can wait and tell the time.
            if not now then raise("scheduler.run cannot wait for a timer without the native task or I/O module", 2) end
            sleep(1)
         else raise("async scheduler deadlock: no runnable task or pending native job", 2) end
      end
   end
   return result(state)
end
-- The runner captures this before executing user code. Keep error objects intact
-- for pcall/try while reporting the suspended coroutine's original stack.
function scheduler.__traceback(failure)
   local state = last_failure
   last_failure = nil
   if state and raw_equal(state.failure, failure) then return state.trace end
end
return scheduler
