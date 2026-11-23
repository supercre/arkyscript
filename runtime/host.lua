-- Typed environment and static imports for an embedding host (arky_runtime.cpp).
-- The CLI applies the same rules in runner.lua from a prepared manifest; a host
-- has no manifest, so modules come from its reader as they are first imported.
-- A host without a reader (a game engine with its own module loaders) imports
-- through require: static imports and legacy require then share package.loaded,
-- and Arky modules loaded that way take this environment themselves (see the
-- compiler's RUNTIME_ENVIRONMENT and package.loaded.tynex_runtime).
local scheduler, read_module, use_require = ...
local load_text, set_environment, protected_call, raise = loadstring, setfenv, pcall, error
local lua_type, set_meta = type, setmetatable

local modules, loading = {}, {}
local environment

-- Generated chunks refer to the runtime through their environment, never through
-- globals a legacy script could replace.
local function load_chunk(code, name)
   local chunk, message = load_text(code, name)
   if not chunk then return nil, message end
   set_environment(chunk, environment)
   return chunk
end

-- A module is loaded once and identified by its name. A module which returns
-- nothing has the value true, as with the CLI's static imports.
local function import(name)
   if lua_type(name) ~= "string" then raise("static import expects a module name", 2) end
   -- The global require at the time of the import, so that a host's own loaders apply.
   if use_require then return require(name) end
   local cached = modules[name]
   if cached then return cached.value end
   if loading[name] then raise("cyclic static import '" .. name .. "'", 2) end
   local code, source = read_module(name)
   if not code then
      raise("unprepared static import '" .. name .. "'" .. (source and (": " .. source) or ""), 2)
   end
   local chunk, message = load_chunk(code, "@" .. source)
   if not chunk then raise(message, 0) end
   loading[name] = true
   local ok, value = protected_call(chunk, name)
   loading[name] = nil
   -- Keep the original error value: typed callers may handle tables with try/except.
   if not ok then raise(value, 0) end
   if value == nil then value = true end
   modules[name] = {value = value}
   return value
end

local public_scheduler = set_meta({}, {__index = scheduler, __metatable = "Tynex scheduler",
   __newindex = function() raise("scheduler methods are read-only", 2) end})
environment = set_meta({__tynex_import = import, __tynex_async_runtime = scheduler,
   scheduler = public_scheduler, fs = scheduler.fs, net = scheduler.net},
   {__index = _G, __newindex = _G})

return {step = scheduler.step, import = import, environment = environment}
