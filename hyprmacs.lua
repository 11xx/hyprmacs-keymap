-- -*- mode: lua; -*-
-- Hyprmacs Lua shim.
--
-- Drop-in replacement for the old `require("hyprmacs-keymap")` helper. It loads
-- the native plugin and re-exposes the same global API (keymap_set, keymap_exec,
-- keymap_configure, bind, bind_exec, ...) so existing modules such as
-- keybinds.lua keep working unchanged.
--
-- The chord engine, prefix tree, modifier tracking and commit rules now live in
-- the native plugin (hl.plugin.hyprmacs.*); this file is only thin glue.

-- hl.plugin.hyprmacs.* only exists once the plugin has loaded. On the very first
-- config evaluation the plugin is still being queued, so this is nil; Hyprland
-- then reloads the config (handlePluginLoads -> reload) and on that second pass
-- the table is present and every binding below registers for real.
local function hm()
    return hl.plugin and hl.plugin.hyprmacs or nil
end

local function file_exists(path)
    local f = io.open(path, "r")
    if f then
        f:close()
        return true
    end
    return false
end

-- Loading strategy:
--   * If managed by hyprpm, the plugin is already loaded (hm() is truthy) and we
--     must NOT call hl.plugin.load — hyprpm owns load state.
--   * Otherwise self-load via hl.plugin.load using, in order: the global
--     HYPRMACS_SO, then ~/.config/hypr/plugins/hyprmacs.so if present.
if not hm() then
    local so = HYPRMACS_SO
    if not so then
        local base = os.getenv("XDG_CONFIG_HOME")
        if not base or base == "" then
            base = (os.getenv("HOME") or "") .. "/.config"
        end
        local guess = base .. "/hypr/plugins/hyprmacs.so"
        if file_exists(guess) then
            so = guess
        end
    end
    if so then
        hl.plugin.load(so)
    end
    -- else: assume hyprpm manages the plugin; bindings register once it loads.
end

-- Fresh start on every (re)load so chords are not registered twice.
do
    local h = hm()
    if h and h.clear then
        h.clear()
    end
end

-- Build a zero-arg action closure from either a function or an hl.dsp.* value.
local function make_action(dispatcher)
    return function()
        if type(dispatcher) == "function" then
            return dispatcher()
        end
        return hl.dispatch(dispatcher)
    end
end

-- strip a `rules` field out of a flags table (used by exec dispatchers)
local function split_rules(flags)
    flags = flags or {}
    local rules = flags.rules
    if rules == nil then
        return flags, nil
    end
    local rest = {}
    for k, v in pairs(flags) do
        if k ~= "rules" then
            rest[k] = v
        end
    end
    return rest, rules
end

-- ===========================================================================
-- public chord API
-- ===========================================================================

function keymap_set(sequence, dispatcher, flags)
    local h = hm()
    if not (h and h.register) then
        return -- first-pass before the plugin is loaded; re-run on reload
    end
    h.register(sequence, make_action(dispatcher), flags)
end

function keymap_exec(sequence, cmd, flags)
    local _, rules = split_rules(flags)
    keymap_set(sequence, hl.dsp.exec_cmd(cmd, rules), flags)
end

function keymap_configure(config)
    local h = hm()
    if h and h.configure then
        h.configure(config or {})
    end
end

-- ===========================================================================
-- final (non-chord) binds — now plain Hyprland binds; the submap hard-reset
-- dance the old helper needed is gone with submaps.
-- ===========================================================================

function bind(keys, dispatcher, flags)
    return hl.bind(keys, dispatcher, flags or {})
end

function bind_exec(keys, cmd, flags)
    local rest, rules = split_rules(flags)
    return hl.bind(keys, hl.dsp.exec_cmd(cmd, rules), rest)
end

-- ===========================================================================
-- legacy submap helpers — kept so older modules don't break. Hyprmacs no longer
-- uses submaps for chords, but Hyprland's submap machinery still exists.
-- ===========================================================================

function reset_submap()
    return hl.dispatch(hl.dsp.submap("reset"))
end

function enter_submap(name)
    return hl.dispatch(hl.dsp.submap(name))
end

function bind_submap(keys, name, flags)
    return hl.bind(keys, hl.dsp.submap(name), flags or {})
end
