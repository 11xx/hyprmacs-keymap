# Hyprmacs

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

### 1. Build

```sh
make                              # -> ./hyprmacs.so
make HYPRLAND_SRC=/path/Hyprland  # alternatively, build against a checkout
```

### 2. Load the plugin

Either with **hyprpm**:

```sh
hyprpm add file:///path/to/hyprmacs-keymap   # or a remote git URL
hyprpm enable hyprmacs
hyprpm reload
```

or **manually** in the running session:

```sh
hyprctl plugin load "$PWD/hyprmacs.so"
```

### 3. Wire the Lua API into your config

Loading the `.so` only provides the engine and the `hl.plugin.hyprmacs.*`
registration functions. The human-facing API (`keymap_set`, `keymap_exec`,
`bind`, …) comes from the shim **`hyprmacs.lua`**, which must be required
**before** any module that defines keybinds:

```lua
require("hyprmacs")   -- defines the API; must come before your keybinds
require("keybinds")   -- your keymap_set / keymap_exec / bind calls
```

Install the helper onto Hyprland's Lua path (the config dir is already on
`package.path`):

```sh
make install-helper                            # -> ~/.config/hypr/hyprmacs.lua
make install-helper LUA_HELPER_DIR=/some/dir   # custom location
```

Then load the plugin one of these ways:

* **hyprpm**: nothing else to do — the shim detects the plugin is already loaded
  and does not call `hl.plugin.load`.
* **self-load**: set `HYPRMACS_SO` before the require, or drop the built
  `hyprmacs.so` into `~/.config/hypr/plugins/`:

  ```lua
  HYPRMACS_SO = "/path/to/hyprmacs.so"
  require("hyprmacs")
  ```

On the first config evaluation the plugin is still being queued, so binding
calls are no-ops; Hyprland then loads the plugin and re-evaluates the config, and
on that pass everything registers. `hyprctl reload` re-registers cleanly.

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
})
```

Conflict policy: duplicate final bindings are allowed by default and run in
registration order; a sequence cannot be both a prefix and a final binding
(always an error).

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

A final binding **without modifiers** commits immediately on press.

A final binding **with modifiers** is recognised at press time but committed
only when either (a) another non-modifier key starts the next chord, or (b) all
of the chord's modifiers are released. This makes overlapping chords
deterministic: holding `Super+Alt` and tapping `Print` locks in `s-M-print`;
releasing the modifiers in any order commits `s-M-print` and never downgrades it
to `M-print`.

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
| `src/Plugin.cpp` | Hyprland glue: key-event hook + `hl.plugin.hyprmacs.*`. |
| `tests/sim.cpp` | Standalone chord-transition simulation. |
| `hyprmacs.lua` | Lua shim exposing the `keymap_set`/`keymap_exec` API. |
| `hyprpm.toml` | hyprpm package manifest. |

## License

Public domain — see [`UNLICENSE`](UNLICENSE).
