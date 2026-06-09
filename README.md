# Hyprmacs

Emacs-like key chords for **Hyprland 0.55+**, implemented as a native C++ plugin.

This replaces the previous Lua-submap implementation
(`reference/hyprmacs-keymap.org`) with an explicit, modifier-aware chord state
machine that runs directly on Hyprland key events — no submap hacks.

> Status: see `reference/` for the behavioral source of truth being migrated.
> Build/migration docs are filled in further down once the implementation lands.

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
| `reference/` | Snapshot of the original Org/Lua implementation. |
