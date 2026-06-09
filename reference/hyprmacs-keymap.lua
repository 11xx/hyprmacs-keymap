-- -*- mode: lua; -*-

function reset_submap()
    return hl.dispatch(hl.dsp.submap("reset"))
end

local suppressed_leaf_presses = {}
local recent_leaf_actions = {}
local keymap_root = { children = {}, path = {}, submap = nil, reset_binds = {} }
local keymap_config = {
    submap_timeout_ms = nil,
    dominated_leaf_suppression_ms = 750,
    strict_duplicates = false,
    report_duplicates = false,
}
local keymap_submap_generation = 0

local function key_id(keys)
    return string.lower((keys:gsub("%s+", "")))
end

local function copy_flags(flags)
    local copied = {}
    for key, value in pairs(flags or {}) do
        copied[key] = value
    end
    return copied
end

local function dispatch_action(dispatcher)
    if type(dispatcher) == "function" then
        return dispatcher()
    end
    return hl.dispatch(dispatcher)
end

local function bind_raw(keys, callback, flags)
    return hl.bind(keys, callback, flags or {})
end

local function full_reset()
    keymap_submap_generation = keymap_submap_generation + 1
    reset_submap()
end

local function keymap_report(text, icon)
    if keymap_config.report_duplicates and hl.notification ~= nil then
        hl.notification.create({
            text = text,
            duration = 5000,
            icon = icon or "info",
        })
    end
end

function bind(keys, dispatcher, flags)
    local id = key_id(keys)
    bind_raw(keys, function()
        local suppressed_count = suppressed_leaf_presses[id] or 0
        if suppressed_count > 0 then
            suppressed_leaf_presses[id] = suppressed_count - 1
            if suppressed_leaf_presses[id] == 0 then
                suppressed_leaf_presses[id] = nil
            end
            return
        end
        full_reset()
        local result = dispatch_action(dispatcher)
        full_reset()
        return result
    end, flags or {})
end

local function split_chord(chord)
    local parts = {}
    for part in chord:gmatch("[^+]+") do
        local trimmed = part:gsub("^%s+", ""):gsub("%s+$", "")
        table.insert(parts, trimmed)
    end
    return parts
end

local function chord_signature(chord)
    local parts = split_chord(chord)
    local modifiers = {}
    for index = 1, #parts - 1 do
        modifiers[parts[index]] = true
    end
    return {
        key = key_id(parts[#parts] or chord),
        modifiers = modifiers,
    }
end

local function modifier_count(modifiers)
    local count = 0
    for _ in pairs(modifiers or {}) do
        count = count + 1
    end
    return count
end

local function modifiers_contain_all(superset, subset)
    for modifier in pairs(subset or {}) do
        if superset[modifier] == nil then
            return false
        end
    end
    return true
end

local function record_leaf_action(signature)
    if keymap_config.dominated_leaf_suppression_ms == nil then
        return
    end

    local key = signature.key
    recent_leaf_actions[key] = recent_leaf_actions[key] or {}
    local record = {
        modifiers = signature.modifiers,
        count = modifier_count(signature.modifiers),
    }
    table.insert(recent_leaf_actions[key], record)

    hl.timer(function()
        local records = recent_leaf_actions[key]
        if records == nil then
            return
        end
        for index, candidate in ipairs(records) do
            if candidate == record then
                table.remove(records, index)
                break
            end
        end
        if #records == 0 then
            recent_leaf_actions[key] = nil
        end
    end, { timeout = keymap_config.dominated_leaf_suppression_ms, type = "oneshot" })
end

local function dominated_leaf_action(signature)
    local records = recent_leaf_actions[signature.key]
    if records == nil then
        return false
    end

    local count = modifier_count(signature.modifiers)
    for _, record in ipairs(records) do
        if record.count > count and modifiers_contain_all(record.modifiers, signature.modifiers) then
            return true
        end
    end
    return false
end

local function bind_keymap_leaf(keys, dispatcher, flags)
    local id = key_id(keys)
    local signature = chord_signature(keys)
    bind_raw(keys, function()
        local suppressed_count = suppressed_leaf_presses[id] or 0
        if suppressed_count > 0 then
            suppressed_leaf_presses[id] = suppressed_count - 1
            if suppressed_leaf_presses[id] == 0 then
                suppressed_leaf_presses[id] = nil
            end
            return
        end
        if dominated_leaf_action(signature) then
            return
        end

        full_reset()
        record_leaf_action(signature)
        local result = dispatch_action(dispatcher)
        full_reset()
        return result
    end, flags or {})
end

function enter_submap(name)
    return hl.dispatch(hl.dsp.submap(name))
end

function bind_submap(keys, name, flags, suppress_count)
    local bind_flags = copy_flags(flags)

    bind_raw(keys, function()
        local id = key_id(keys)
        if type(suppress_count) == "function" then
            suppressed_leaf_presses[id] = suppress_count()
        else
            suppressed_leaf_presses[id] = suppress_count or 1
        end

        reset_submap()
        keymap_submap_generation = keymap_submap_generation + 1
        local timeout_generation = keymap_submap_generation
        local result = enter_submap(name)
        if keymap_config.submap_timeout_ms ~= nil then
            hl.timer(function()
                if keymap_submap_generation == timeout_generation then
                    full_reset()
                end
            end, { timeout = keymap_config.submap_timeout_ms, type = "oneshot" })
        end
        return result
    end, bind_flags)
end

function keymap_configure(config)
    for key, value in pairs(config or {}) do
        keymap_config[key] = value
    end
end

local function normalize_key_name(key)
    local aliases = {
        [" "] = "space",
        ["RET"] = "return",
        ["RETURN"] = "return",
        ["ESC"] = "escape",
        ["ESCAPE"] = "escape",
        ["SPC"] = "space",
        ["SPACE"] = "space",
    }
    local upper = string.upper(key)
    if aliases[upper] then
        return aliases[upper]
    end
    if #key == 1 and key:match("%a") then
        return upper
    end
    return key
end

local function parse_emacs_modifier(modifier)
    if modifier == "s" then
        return "SUPER"
    elseif modifier == "C" then
        return "CTRL"
    elseif modifier == "M" then
        return "ALT"
    elseif modifier == "S" then
        return "SHIFT"
    end
    return nil
end

local function parse_emacs_chord(chord)
    if chord:find("%+") then
        return chord
    end

    local parts = {}
    for part in chord:gmatch("[^-]+") do
        table.insert(parts, part)
    end

    if #parts == 0 then
        error("keymap_set: invalid chord " .. chord)
    end
    if #parts == 1 then
        return normalize_key_name(parts[1])
    end

    local seen_mods = {}
    for index = 1, #parts - 1 do
        local parsed = parse_emacs_modifier(parts[index])
        if parsed == nil then
            return chord
        end
        seen_mods[parts[index]] = parsed
    end

    local mods = {}
    for _, modifier in ipairs({ "C", "M", "s", "S" }) do
        if seen_mods[modifier] ~= nil then
            table.insert(mods, seen_mods[modifier])
        end
    end
    table.insert(mods, normalize_key_name(parts[#parts]))
    return table.concat(mods, " + ")
end

local function parse_key_sequence(sequence)
    if type(sequence) == "table" then
        local chords = {}
        for _, chord in ipairs(sequence) do
            table.insert(chords, parse_emacs_chord(chord))
        end
        return chords
    end

    local chords = {}
    for chord in sequence:gmatch("%S+") do
        table.insert(chords, parse_emacs_chord(chord))
    end
    return chords
end

local function submap_name(path)
    local name = table.concat(path, "__")
    name = name:gsub("[^%w]+", "_"):gsub("^_+", ""):gsub("_+$", "")
    return "keymap_" .. name
end

local function in_submap(node, callback)
    if node.submap == nil then
        callback()
    else
        hl.define_submap(node.submap, callback)
    end
end

local function disable_binds(binds)
    for _, bind in ipairs(binds or {}) do
        bind:set_enabled(false)
    end
end

local function refresh_reset_binds(node)
    if node.submap == nil then
        return
    end

    disable_binds(node.reset_binds)
    node.reset_binds = {}

    hl.define_submap(node.submap, function()
        table.insert(node.reset_binds, bind_raw("escape", function()
            full_reset()
        end))
        table.insert(node.reset_binds, bind_raw("catchall", function()
            full_reset()
        end))
    end)
end

local function same_key_action_count(node, chord)
    local child = node.children[chord]
    if child ~= nil and child.actions ~= nil and #child.actions > 0 then
        return #child.actions
    end
    return 1
end

local function ensure_prefix(parent, chord)
    local child = parent.children[chord]
    if child ~= nil then
        return child
    end

    local path = {}
    for _, item in ipairs(parent.path) do
        table.insert(path, item)
    end
    table.insert(path, chord)

    child = {
        children = {},
        path = path,
        submap = submap_name(path),
        reset_binds = {},
    }
    parent.children[chord] = child

    in_submap(parent, function()
        bind_submap(chord, child.submap, nil, function()
            return same_key_action_count(child, chord)
        end)
    end)
    refresh_reset_binds(parent)

    return child
end

function keymap_set(sequence, dispatcher, flags)
    local chords = parse_key_sequence(sequence)
    if #chords == 0 then
        error("keymap_set: empty key sequence")
    end

    local node = keymap_root
    for index, chord in ipairs(chords) do
        if index < #chords then
            local child = node.children[chord]
            if child ~= nil and child.actions ~= nil and #child.actions > 0 then
                error("keymap_set: " .. table.concat(chords, " ") .. " conflicts with final binding " .. table.concat(child.path, " "))
            end
            node = ensure_prefix(node, chord)
        else
            local existing = node.children[chord]
            if existing ~= nil and next(existing.children) ~= nil then
                error("keymap_set: " .. table.concat(chords, " ") .. " conflicts with prefix binding " .. table.concat(existing.path, " "))
            end
            if existing ~= nil and existing.actions ~= nil and #existing.actions > 0 then
                if keymap_config.strict_duplicates then
                    error("keymap_set: duplicate binding " .. table.concat(chords, " "))
                end
                keymap_report("Hyprmacs duplicate final binding: " .. table.concat(chords, " "), "info")
            end

            local path = {}
            for _, item in ipairs(node.path) do
                table.insert(path, item)
            end
            table.insert(path, chord)

            local leaf = existing or {
                children = {},
                path = path,
                submap = nil,
                reset_binds = {},
                actions = {},
            }
            leaf.path = path
            leaf.actions = leaf.actions or {}
            table.insert(leaf.actions, dispatcher)
            node.children[chord] = leaf

            in_submap(node, function()
                bind_keymap_leaf(chord, dispatcher, flags)
            end)
            refresh_reset_binds(node)
        end
    end
end

function keymap_exec(sequence, cmd, flags)
    local bind_flags = flags or {}
    local rules = bind_flags.rules
    if rules ~= nil then
        bind_flags = {}
        for key, value in pairs(flags) do
            if key ~= "rules" then
                bind_flags[key] = value
            end
        end
    end
    keymap_set(sequence, hl.dsp.exec_cmd(cmd, rules), bind_flags)
end

function bind_exec(keys, cmd, flags)
    local bind_flags = flags or {}
    local rules = bind_flags.rules
    if rules ~= nil then
        bind_flags = {}
        for key, value in pairs(flags) do
            if key ~= "rules" then
                bind_flags[key] = value
            end
        end
    end
    bind(keys, hl.dsp.exec_cmd(cmd, rules), bind_flags)
end
