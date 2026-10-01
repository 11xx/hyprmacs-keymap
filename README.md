# hyprmacs-keymap

Emacs-style multi-key sequences for Hyprland.

Hyprland binds one chord to one action. A sequence such as `s-x e e` has to be
built from submaps, which are modal: each one stays active until something
leaves it, and releasing modifiers in a different order can resolve to a
different bind.

hyprmacs-keymap is a Hyprland plugin with a prefix-tree chord engine. A binding
fires on key-down the moment its sequence is complete; the engine waits only
while the keys typed so far are a prefix of a longer sequence. A key that starts
no sequence passes through untouched, so ordinary binds and typing keep
working. Inside a sequence, a key that does not continue it cancels the
sequence and is swallowed, as in Emacs.

```lua
keymap_exec("s-x space", "wofi --show drun")   -- Super+X, then Space
keymap_exec("s-x e e",   "emacsclient -c")     -- Super+X, then E, then E
keymap_set("s-f s-f", hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle" }))
keymap_exec("M-print",   'grim -g "$(slurp)"') -- a single chord works too
```

## Install

Requires Hyprland 0.56 or later with the Lua config. The plugin ABI is tied
to the exact Hyprland build, so rebuild after every Hyprland update
(`hyprpm update`); a build for another Hyprland commit refuses to load.
Building needs a C++26 compiler, PUC Lua 5.5 headers (the Lua Hyprland
embeds) and xkbcommon.

```sh
hyprpm add https://github.com/11xx/hyprmacs-keymap
hyprpm enable hyprmacs-keymap
make install-helper     # copies hyprmacs-keymap.lua to ~/.config/hypr/
```

The plugin exposes `hl.plugin.hyprmacs_keymap.*`; the Lua helper turns that
into the `keymap_*` API. Require it before your binds:

```lua
require("hyprmacs-keymap")
require("keybinds")   -- your keymap_set / keymap_exec calls
```

Without hyprpm, `make` builds `./hyprmacs-keymap.so` (`HYPRLAND_SRC=` points
it at a Hyprland checkout), and the helper loads the file named by
`HYPRMACS_KEYMAP_SO`, or `~/.config/hypr/plugins/hyprmacs-keymap.so` if it
exists.

## Keys and API

A sequence is space-separated chords; modifiers come before the key: `s-`
Super, `C-` Ctrl, `M-` Alt, `S-` Shift. Modifier order does not matter, and
modifiers held for one chord do not carry into the next (`s-x space` and
`s-x s-space` differ). The key is any xkbcommon keysym name, `-` for minus
(`C--`), `code:NN` for a raw keycode, or one of `SPC`, `RET`, `ESC`.
Hyprland's own `"SUPER + F"` form is accepted as well. Keys resolve the way
Hyprland's binds do: by the first keyboard layout, or the active one on a
device with `resolve_binds_by_sym`. A device with `keybinds = false` never
triggers a sequence, and none run while the session is locked.

```lua
keymap_set(seq, dispatcher, flags)    -- a function or an hl.dsp.* value
keymap_exec(seq, command, flags)      -- flags.rules goes to exec
keymap_set("s-r f", hl.dsp.window.resize({ x = 10, y = 0, relative = true }),
           { repeating = true })    -- repeats while F is held

keymap_configure({
    submap_timeout_ms = 5000,   -- drop a half-typed prefix this long after
                                -- the last key event (default: off)
    strict_duplicates = true,   -- error on a duplicate final binding
    report_duplicates = true,   -- notify on one, but allow it
    debug             = true,   -- log chord events to /tmp/hyprmacs-keymap.log
})
```

A duplicate final binding runs every action in registration order unless
`strict_duplicates` is set. A sequence that is both a prefix and a final
binding is always an error. Settings and bindings reset on every config
reload. Actions run like Hyprland's own Lua binds: an error shows a
notification and a runaway action is cut off. `debug` logs only modifiers and
keys that touch a sequence, never plain typing, and shows a reminder
notification every 30 seconds while it is on. `bind` and `bind_exec` register
plain Hyprland binds, for mouse buttons and the other binds a sequence step
cannot use.

With `kernel.sysrq` non-zero, the kernel holds Alt+PrtSc (magic SysRq) until
Alt is released, so an `M-print` binding fires on release. The plugin warns
when such a binding is registered; `sysctl kernel.sysrq=0` avoids it.

`make test` runs the chord engine's standalone simulation, which needs only
xkbcommon. `reference/` holds a pure-Lua, submap-based version of the same API
and the author's own keybinds file as a full example; the commands it runs are
the author's personal scripts.

## License

Public domain; see [`UNLICENSE`](UNLICENSE).
