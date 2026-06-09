# hyprmacs-keymap

Emacs-like, modifier-aware key chords for **Hyprland 0.55+**, as a native C++
plugin.

Define multi-step key sequences (`s-x` then `e` then `e`) and exact
modifier-aware chords. The plugin runs a single deterministic chord state
machine directly on Hyprland key events — no submaps, no settling timers.

```lua
keymap_exec("s-x space", "wofi --show drun")   -- press s-x, then space
keymap_exec("s-x e e",   "emacsclient -c")     -- s-x, then e, then e
keymap_set("s-f s-f", hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle" }))
keymap_exec("M-print",   'grim -g "$(slurp)"') -- Alt+Print, single chord
```

## Requirements

* Hyprland **0.55+** (Lua config). Plugin ABI is tied to the exact Hyprland
  commit — **rebuild after every Hyprland update**.
* Build toolchain matching Hyprland's (C++26-capable compiler).
* Hyprland's headers, via the `hyprland` pkg-config (`hyprpm update` provides
  them) or a Hyprland source checkout.
* PUC **Lua 5.5** dev package and `xkbcommon` (the plugin shares Hyprland's Lua).

## Install

Two pieces: the plugin (`.so`) and the Lua helper (`hyprmacs-keymap.lua`). hyprpm
builds and loads the `.so`; the helper provides the `keymap_set`/`keymap_exec`
API and lives in your config.

### 1. Add the plugin with hyprpm

```sh
hyprpm add file:///path/to/hyprmacs-keymap   # or a remote git URL
hyprpm enable hyprmacs-keymap
hyprpm reload
```

hyprpm runs the build itself — you do **not** need to run `make`. After changing
the source, `hyprpm update` rebuilds and reloads in one step (also rebuild after
every Hyprland update).

### 2. Install the Lua helper

The `.so` only exposes `hl.plugin.hyprmacs_keymap.*`; the friendly API
(`keymap_set`, `keymap_exec`, `bind`, …) comes from `hyprmacs-keymap.lua`. Drop
it on Hyprland's Lua path (the config dir already is one) — this is just a copy,
not a build:

```sh
make install-helper                            # -> ~/.config/hypr/hyprmacs-keymap.lua
make install-helper LUA_HELPER_DIR=/some/dir   # or just: cp hyprmacs-keymap.lua ~/.config/hypr/
```

### 3. Require it before your keybinds

```lua
require("hyprmacs-keymap")   -- defines the API; must come before your keybinds
require("keybinds")          -- your keymap_set / keymap_exec / bind calls
```

Under hyprpm the helper sees the plugin is already loaded and does nothing else.
On the very first config evaluation the plugin is still being queued, so binding
calls are no-ops; Hyprland loads it, re-evaluates, and everything registers on
that pass. `hyprctl reload` re-registers cleanly.

### Manual build (development only)

You only need `make` if you load the `.so` yourself instead of through hyprpm —
e.g. heavy hands-on development:

```sh
make                               # -> ./hyprmacs-keymap.so
make HYPRLAND_SRC=/path/Hyprland   # build against a Hyprland checkout
hyprctl plugin load "$PWD/hyprmacs-keymap.so"
```

To have the helper self-load it (no hyprpm), point it at the file before the
require — otherwise it auto-loads `~/.config/hypr/plugins/hyprmacs-keymap.so` if
that exists:

```lua
HYPRMACS_KEYMAP_SO = "/path/to/hyprmacs-keymap.so"
require("hyprmacs-keymap")
```

## Key syntax

A sequence is space-separated chords; within a chord, modifiers precede the key.

| Prefix | Modifier |
|--------|----------|
| `s-`   | Super    |
| `C-`   | Ctrl     |
| `M-`   | Alt / Meta |
| `S-`   | Shift    |

* `s-c` = Super+C; `C-c` = Ctrl+C; `s` alone = the `s` key.
* Raw Hyprland syntax with `+` is also accepted: `"SUPER + F"`.
* Aliases: `SPC`/`SPACE`→space, `RET`/`RETURN`→return, `ESC`/`ESCAPE`→escape.
* Matching is case-insensitive (like Hyprland binds); use `S-` for Shift.
* Modifier **order doesn't matter**: `s-M-x` and `M-s-x` are the same chord —
  modifiers are an unordered set, as in Emacs. Internally a chord is
  `(modifier bitmask, key)`, so there's no canonical-order requirement. (The old
  Lua helper had to sort modifiers into a canonical `C M s S` order for
  Hyprland's string-based bind matching; the native engine doesn't.)
* Held modifiers are **not** carried into later chords: `s-x space` and
  `s-x s-space` are different bindings.

## API

| Function | Purpose |
|----------|---------|
| `keymap_set(seq, dispatcher, flags?)` | Register a chord. `dispatcher` is a function or an `hl.dsp.*` value. |
| `keymap_exec(seq, command, flags?)` | Register a chord that runs a shell command. `flags.rules` is forwarded to `exec`. |
| `keymap_configure(opts)` | Set engine options (see below). |
| `bind(keys, dispatcher, flags?)` | A plain (non-chord) Hyprland bind. |
| `bind_exec(keys, command, flags?)` | A plain bind that runs a command. |
| `enter_submap` / `reset_submap` / `bind_submap` | Thin wrappers over Hyprland submaps, kept for compatibility. |

## Configuration

```lua
keymap_configure({
    submap_timeout_ms = 5000,  -- reset a half-typed prefix after N ms (default: off)
    strict_duplicates = false, -- error on duplicate final bindings
    report_duplicates = false, -- notify (but still allow) duplicate finals
    debug             = false, -- diagnostics to /tmp/hyprmacs-keymap.log (default: off)
})
```

Conflict policy: duplicate final bindings are allowed by default and run in
registration order; a sequence cannot be both a prefix and a final binding
(always an error).

Settings are declarative — they reset to defaults on every reload, so removing a
line (e.g. `debug`) turns it back off. `debug` logs only **chord-relevant**
events (modifiers, captured/committed keys, prefix progress) to
`/tmp/hyprmacs-keymap.log` (owner-only); plain typed text is never logged, so it
is not a keylogger. While it is on, the plugin shows a loud red notification and
repeats it every 30s as a reminder. Leave it off unless diagnosing.

## How it works

One function hook on `CKeybindManager::onKeyEvent` feeds every key into the
engine, which tracks the pressed modifiers and the active node of a chord prefix
tree. Per key:

* modifier keys update the held-modifier set and always pass through;
* a non-modifier press forms the chord `(held modifiers, key)` and walks the
  tree — unknown at idle passes through (normal binds and typing keep working),
  unknown inside a prefix aborts the sequence, a prefix advances, a final
  binding commits.

### Commit timing

Matching is **eager**, like Emacs: a binding fires the instant the key sequence
completes a final binding — on key-**down**, even if modifiers are still held.
The engine only waits while the current sequence is a *prefix* of a longer one.
Since a sequence can't be both a prefix and a final binding, there's never any
ambiguity about whether to fire now or wait.

So `keymap_set("s-x s-c", …)` runs the moment `c` goes down (with Super still
held); you don't have to release anything. The modifiers held *at the key press*
pick the chord, so `s-M-j` and `M-j` stay distinct with no downgrades.

If a binding is registered more than once it runs every action in registration
order (handy for chaining); set `strict_duplicates = true` to forbid that and
keep bindings single/unitary.

## Gotcha: Alt+PrtSc and magic-SysRq

`PrtSc` is `KEY_SYSRQ`, and `Alt+SysRq` is the kernel's magic-SysRq combo. When
`kernel.sysrq != 0` the kernel **intercepts `Alt+PrtSc` before it reaches
Hyprland**, buffering the keypress until you release Alt. The effect: a chord
like `M-print` / `s-M-print` fires on *Alt-release* (and `s-M-print` can
mis-resolve to `M-print` depending on release order). This is not the chord
engine — the press never reaches the plugin — so the plugin can't fix it; it
only warns. Options:

* `sudo sysctl kernel.sysrq=0` (and persist via `/etc/sysctl.d/`), or
* remap `PrtSc` off `KEY_SYSRQ` (e.g. to `KEY_PRINT`) in your remapper and put
  REISUB on another key, or
* don't combine `Alt` with `PrtSc` (`print`, `S-print`, `C-print`, `s-print …`
  are unaffected).

When `kernel.sysrq != 0` and you register an `Alt`+`Print` binding, the plugin
logs a warning to `/tmp/hyprmacs-keymap.log` and shows a one-time notification.

## Testing

The chord engine is independent of Hyprland and has a standalone simulation:

```sh
make test   # builds & runs tests/sim.cpp (needs only xkbcommon)
```

## Layout

| Path | Purpose |
|------|---------|
| `src/Core.hpp` | Shared types (modifier masks, `Chord`, `ActionId`). |
| `src/KeyParser.{hpp,cpp}` | Parse key syntax → `Chord` steps. |
| `src/PrefixTree.{hpp,cpp}` | Chord prefix tree + duplicate/conflict policy. |
| `src/ChordStateMachine.{hpp,cpp}` | The deterministic chord engine. |
| `src/Plugin.cpp` | Hyprland glue: key-event hook + `hl.plugin.hyprmacs_keymap.*`. |
| `tests/sim.cpp` | Standalone chord-transition simulation. |
| `hyprmacs-keymap.lua` | Lua shim exposing the `keymap_set`/`keymap_exec` API. |
| `hyprpm.toml` | hyprpm package manifest. |

## License

Public domain — see [`UNLICENSE`](UNLICENSE).
