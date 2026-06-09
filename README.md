# Hyprmacs

Emacs-like, modifier-aware key chords for **Hyprland 0.55+**, implemented as a
native C++ plugin.

It replaces the previous Lua-submap implementation
(`reference/hyprmacs-keymap.org`, the behavioural source of truth) with a single
explicit chord **state machine** that runs directly on Hyprland key events — no
submap hacks, no settling timers.

```lua
keymap_exec("s-x space", "$(tofi-run)")     -- s-x then space
keymap_exec("s-x e e", "emacsclient-def")   -- s-x then e then e
keymap_set("s-f s-f", hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle" }))
keymap_exec("s-M-print", "shotdrag")        -- Super+Alt+Print
```

## Why native

Hyprland 0.55 moved its config to Lua. The old helper drove Hyprland *submaps*
as throwaway prefix maps and fought several side effects:

* a 750 ms "settling window" to decide between overlapping chords like
  `M-print` and `s-M-print` while modifiers were being released;
* same-key press suppression bookkeeping because Lua binds don't stop the
  in-progress key scan;
* constant re-adding of `escape`/`catchall` reset binds in registration order.

The native plugin throws all of that away. It tracks pressed modifiers and the
active prefix node itself, so each chord is resolved **exactly** from the
modifiers held at the moment a non-modifier key is pressed.

## How it works

One function hook on `CKeybindManager::onKeyEvent` feeds every key into the
engine (`src/ChordStateMachine.cpp`). State:

* `held` — currently-pressed modifiers (Super/Ctrl/Alt/Shift);
* `current` — position in the chord **prefix tree** (`src/PrefixTree.cpp`);
* `pending` — a recognised-but-not-yet-committed modified final binding.

Per key:

* **modifier** keys update `held` and always pass through;
* a **non-modifier press** forms the chord `(held, key)` and walks the tree:
  * unknown at idle → passed through (normal binds / typing still work);
  * unknown inside a prefix → aborts and is swallowed;
  * a prefix → becomes the new `current`;
  * a final binding → committed (see below).

### Commit rules (the important part)

A final binding **with no modifiers** commits immediately on press.

A final binding **with modifiers** is *recognised* at key-press time but the
command is *committed* only when either:

1. another non-modifier key starts the next chord, **or**
2. **all** of the chord's modifiers have been released.

This is what makes overlapping chords deterministic. Holding `Super+Alt` and
tapping `Print` locks in `s-M-print`; releasing Super then Alt (or Alt then
Super) commits `s-M-print` — it is **never** downgraded to `M-print`. Releasing
modifiers can never change which chord was recognised.

Because commit is driven by real key releases, the old `modified_leaf_commit_delay_ms`
timer is gone (the option is still accepted but ignored).

## Build

Requires the matching Hyprland dev headers (the `hyprland` pkg-config, provided
by `hyprpm`/your distro) and the same toolchain Hyprland was built with.

```sh
make            # -> ./hyprmacs.so   (default; also what hyprpm runs)
make test       # build & run the standalone chord simulation (no Hyprland needed)
make install    # alias for `make all` (builds ./hyprmacs.so in the repo dir)
make HYPRLAND_SRC=/tmp/Hyprland   # build against a Hyprland checkout instead
```

> Hyprland 0.55 embeds **PUC Lua 5.5** (not LuaJIT). The Makefile builds against
> `pkg-config lua` so the plugin shares Hyprland's `lua_State`. Rebuild whenever
> Hyprland updates — plugin ABI is tied to the exact Hyprland commit.

## Install

### With hyprpm (recommended)

`hyprmacs-keymap` is the repository name; `hyprmacs` is the plugin name in
`hyprpm list`/`enable`.

```sh
hyprpm update
hyprpm add file:///path/to/hyprmacs-keymap   # or a remote URL
hyprpm enable hyprmacs
hyprpm reload
```

hyprpm builds (`make all`) and loads the plugin for you. **Do not** call
`hl.plugin.load` in your config in this mode — the shim detects that the plugin
is already loaded and skips self-loading.

### Manual / local

```sh
make                                  # -> ./hyprmacs.so
hyprctl plugin load "$PWD/hyprmacs.so"   # try it in the running session
```

For config-managed loading without hyprpm, point the shim at the `.so`:

```lua
HYPRMACS_SO = "/path/to/hyprmacs-keymap/hyprmacs.so"
require("hyprmacs")
```

If `HYPRMACS_SO` is unset, the shim also auto-loads
`~/.config/hypr/plugins/hyprmacs.so` when that file exists.

## Migration from the old Lua helper

The plugin keeps the **same public API**, so migration is two line changes.

1. Install the plugin (see **Install** above — hyprpm, or build `./hyprmacs.so`
   and set `HYPRMACS_SO`).

2. In your Lua config, replace the helper require with the shim. In
   `hyprland.lua`:

   ```diff
   - require("hyprmacs-keymap")
   + require("hyprmacs")
   ```

   Copy `hyprmacs.lua` next to your other config modules (e.g.
   `~/.config/hypr/hyprmacs.lua`). It loads the plugin (or, under hyprpm,
   detects it is already loaded) and re-defines the global API.

3. Everything in `keybinds.lua` stays the same:
   `keymap_set`, `keymap_exec`, `keymap_configure`, `bind`, `bind_exec`,
   `bind_submap`, `enter_submap`, `reset_submap` all still exist.

On first evaluation the plugin is still being queued, so the binding calls are
no-ops; Hyprland loads the plugin and reloads the config, and on that second
pass everything registers. A manual `hyprctl reload` re-registers cleanly (the
plugin clears its previous state).

Keep `hyprmacs-keymap.org`/`.lua` until you have verified the plugin in a live
session — `reference/` holds a snapshot either way.

## Key syntax (unchanged)

Space-separated chords; per chord, modifiers then the final key:

| Token | Meaning |
|-------|---------|
| `s-`  | Super   |
| `C-`  | Ctrl    |
| `M-`  | Alt / Meta |
| `S-`  | Shift   |

* `s-c` = Super+C, `C-c` = Ctrl+C, `s` alone = the `s` key.
* Raw Hyprland syntax with `+` is also accepted: `keymap_set("SUPER + F", ...)`.
* Aliases: `SPC`/`SPACE`→space, `RET`/`RETURN`→return, `ESC`/`ESCAPE`→escape.
* Held modifiers are **not** carried into later chords — `s-x space` and
  `s-x s-space` are different bindings.

## Configuration

```lua
keymap_configure({
    submap_timeout_ms  = 5000,  -- reset a half-typed prefix after N ms (default: off)
    strict_duplicates  = false, -- error on duplicate final bindings
    report_duplicates  = false, -- notify (but still allow) duplicate finals
})
```

Duplicate / conflict policy matches the Org spec: duplicate finals are allowed
by default (commands run in registration order); a sequence cannot be both a
prefix and a final (always an error).

## Deviations from the Org behaviour

* Overlapping modified chords are resolved by **modifier release**, not a 750 ms
  timer — deterministic and instant. `modified_leaf_commit_delay_ms` is ignored.
* `bind`/`bind_exec` are plain `hl.bind` calls now (no submap reset dance).
* `bind_submap`/`enter_submap`/`reset_submap` remain for compatibility but are
  not used by the chord engine.
* Per-bind flags meaningful only to Hyprland binds (`repeating`, `locked`, …)
  are not applied to chord *leaves* (chords fire once on commit). `rules` is
  honoured for `keymap_exec`.

## Layout

| Path | Purpose |
|------|---------|
| `src/Core.hpp` | Shared types (modifier masks, `Chord`, `ActionId`). |
| `src/KeyParser.{hpp,cpp}` | Parse Hyprmacs key syntax → `Chord` steps. |
| `src/PrefixTree.{hpp,cpp}` | Chord prefix tree + duplicate/conflict policy. |
| `src/ChordStateMachine.{hpp,cpp}` | The deterministic chord engine. |
| `src/Plugin.cpp` | Hyprland glue: key-event hook + `hl.plugin.hyprmacs.*`. |
| `tests/sim.cpp` | Standalone simulation harness for chord transitions. |
| `hyprmacs.lua` | Lua shim preserving the `keymap_set`/`keymap_exec` API. |
| `hyprpm.toml` | hyprpm package manifest. |
| `reference/` | Snapshot of the original Org/Lua implementation. |

## License

Public domain — see [`UNLICENSE`](UNLICENSE).
