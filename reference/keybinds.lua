-- A real personal keybinds file, kept as a full usage example. The emacsclient-*,
-- screenshot-jxl and shotdrag commands are the author's own scripts; substitute
-- your own launchers.

-- #### Binds ####
-- See https://wiki.hyprland.org/Configuring/Keywords/ for more
local browser = "firefox"

bind("SUPER + G", hl.dsp.submap("reset"), { submap_universal = true })

bind("SUPER + T", hl.dsp.window.float({ action = "toggle" }))

-- hl.dsp.layout(...) wraps Hyprland's native layoutmsg dispatcher.
bind("SUPER + R", hl.dsp.layout("rotatesplit 90"))
bind("SUPER + SHIFT + R", hl.dsp.layout("rotatesplit 270"))
bind("SUPER + S", hl.dsp.layout("swapsplit"))

-- [[https://github.com/hyprwm/Hyprland/issues/102][Submap of key bindings · Issue #102 · hyprwm/Hyprland]]
-- [[https://wiki.hyprland.org/Configuring/Uncommon-tips--tricks/#disabling-keybinds-with-one-master-keybind][Uncommon Tips & Tricks | Hyprland Wiki]]

-- [[https://www.reddit.com/r/hyprland/comments/12q4xib/comment/jgqd0gw/][Activating lock/dpms from script with swayidle : r/hyprland]]
bind("SUPER + ALT + pause", hl.dsp.dpms({ action = "disable" }), { locked = true })

keymap_exec("s-x space", "$(tofi-run)")
keymap_exec("s-x d", "dolphin")
keymap_set("s-x s-c", hl.dsp.window.close()) -- for kill with M-x M-c
keymap_exec("s-x f", browser)
keymap_exec("s-x z", "zen-browser")
keymap_exec("s-x v", "vivaldi")
keymap_exec("s-x e e", "emacsclient-def")
keymap_exec("s-x e c", "emacsclient-code")

bind_exec("SUPER + return", "alacritty")
bind_exec("SUPER + backspace", "foot")

bind("SUPER + H", hl.dsp.focus({ direction = "l" }))
bind("SUPER + J", hl.dsp.focus({ direction = "d" }))
bind("SUPER + K", hl.dsp.focus({ direction = "u" }))
bind("SUPER + L", hl.dsp.focus({ direction = "r" }))
bind("SUPER + P", hl.dsp.window.cycle_next({ prev = true }))
bind("SUPER + N", hl.dsp.window.cycle_next({ next = true }))

function bubblemv(direction)
   return function()
      return hl.plugin.bubblemv.move(direction)
   end
end
bind("SUPER + SHIFT + H", bubblemv("left"))
bind("SUPER + SHIFT + J", bubblemv("down"))
bind("SUPER + SHIFT + K", bubblemv("up"))
bind("SUPER + SHIFT + L", bubblemv("right"))

bind("SUPER + ALT + B", hl.dsp.window.resize({ x = -10, y = 0, relative = true }), { repeating = true })
bind("SUPER + ALT + N", hl.dsp.window.resize({ x = 0, y = 10, relative = true }), { repeating = true })
bind("SUPER + ALT + P", hl.dsp.window.resize({ x = 0, y = -10, relative = true }), { repeating = true })
bind("SUPER + ALT + F", hl.dsp.window.resize({ x = 10, y = 0, relative = true }), { repeating = true })

-- Switch workspaces with mainMod + [0-9]
-- "exec, hypr-workspace" deprecated by https://wiki.hyprland.org/Configuring/Dispatchers/#:~:text=focusworkspaceoncurrentmonitor
for i = 1, 10 do
   local key = i % 10
   bind("SUPER + " .. key, hl.dsp.focus({ workspace = i, on_current_monitor = true }))
   bind("SUPER + ALT + " .. key, hl.dsp.focus({ workspace = i }))
   bind("SUPER + SHIFT + " .. key, hl.dsp.window.move({ workspace = i, follow = false }))
end

bind("SUPER + SHIFT + S", hl.dsp.workspace.swap_monitors({ monitor1 = 0, monitor2 = 1 }))

-- **** Scratchpads
-- aka special workspaces
bind("SUPER + SHIFT + equal", hl.dsp.window.move({ workspace = "special:scratch", follow = false }))
bind("SUPER + equal", hl.dsp.workspace.toggle_special("scratch"))
bind("SUPER + SHIFT + minus", hl.dsp.window.move({ workspace = "special:minimized", follow = false }))
bind("SUPER + minus", hl.dsp.workspace.toggle_special("minimized"))

-- Scroll through existing workspaces with mainMod + scroll
bind("SUPER + mouse_down", hl.dsp.focus({ workspace = "e+1" }))
bind("SUPER + mouse_up", hl.dsp.focus({ workspace = "e-1" }))

-- Move/resize windows with mainMod + LMB/RMB and dragging
bind("SUPER + mouse:272", hl.dsp.window.drag(), { mouse = true })
bind("SUPER + mouse:273", hl.dsp.window.resize(), { mouse = true })

-- ytdotool debounce script for faulty middle click
-- bind_exec("mouse:274", "ydotool click -D 200 0xC2 --repeat=1")
-- middle-click-debouce

bind("SUPER + CTRL + P", hl.dsp.window.pin())

-- ## Layout binds
-- bind("SUPER + F", hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle" }))

-- https://github.com/hyprwm/Hyprland/issues/7770
bind("SUPER + F11", hl.dsp.window.fullscreen_state({ internal = 2, client = -1, action = "toggle" }))

keymap_set("s-f s-f", hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle" }))

keymap_set("s-w t", hl.dsp.group.toggle())
keymap_set("s-w h", hl.dsp.window.move({ into_group = "l" }))
keymap_set("s-w j", hl.dsp.window.move({ into_group = "d" }))
keymap_set("s-w k", hl.dsp.window.move({ into_group = "u" }))
keymap_set("s-w l", hl.dsp.window.move({ into_group = "r" }))
bind("SUPER + CTRL + ALT + N", hl.dsp.group.next())

keymap_exec("print", "screenshot-jxl")
keymap_exec("S-print", "screenshot-jxl --select")
keymap_exec("C-print", "screenshot-jxl --select --copy image")
keymap_exec("M-print", "screenshot-jxl --lossy")
keymap_exec("s-M-print", "shotdrag")
keymap_exec("s-print d", "shotdrag")
keymap_exec("s-print w", "screenshot-jxl --window")

-- color picker: grim -g "$(slurp -p)" -t ppm - | magick - -format '%[pixel:p{0,0}]' txt:-
keymap_exec("s-C-M-o c", "hyprpicker -dbra -u 100")

bind("SUPER + CTRL + ALT + pause", hl.dsp.exit())

bind("SUPER + O", hl.dsp.focus({ monitor = "+1" }))
bind("SUPER + SHIFT + O", hl.dsp.window.move({ monitor = "+1" }))
-- exec, hyprctl dispatch movewindow mon:"$(hyprctl monitors -j | jq -r '.[] | select(.focused == false).id')"

-- Notification controls
bind_exec("SUPER + CTRL + ALT + S", "swaync-client --open-panel")

-- Switch keyboard layouts, find names with `hyprctl devices'
-- bind_exec("control_r", "hyprctl switchxkblayout \"$(pgrep kmonad && printf 'kmonad-uinput-sink' || printf 'sino-wealth-usb-keyboard')\" next")
bind_exec("SUPER + ALT + menu", "hyprctl switchxkblayout \"$(pgrep -x xremap >/dev/null && printf 'xremap' || printf 'sino-wealth-usb-keyboard')\" next")

bind_exec("SUPER + CTRL + ALT + period", "tofimoji")

bind_exec("SUPER + CTRL + ALT + M", "terminal -w 120 -h 20 --float wiremix", { rules = { float = true, stay_focused = true } })

bind_exec("ALT + F4", "notify-send lmao")

bind_exec("SUPER + CTRL + ALT + D", "aria2-dragdrop")

-- Save last 60 seconds
bind_exec("SUPER + ALT + SPACE", "gpu-screen-recorder-replay-save")

-- Pause/resume replay buffer
bind_exec("SUPER + ALT + home", "gpu-screen-recorder-replay-toggle")

-- Stop replay buffer entirely
bind_exec("SUPER + ALT + end", "gpu-screen-recorder-replay-stop")

bind_exec("SUPER + ALT + F1", "gpu-screen-recorder-replay-save-10")
bind_exec("SUPER + ALT + F2", "gpu-screen-recorder-replay-save-30")
bind_exec("SUPER + ALT + F3", "gpu-screen-recorder-replay-save-60")

bind_exec("SUPER + ALT + tab", "gpu-screen-recorder-replay-toggle-recording")

-- Media keys
-- the recommended in http://wiki.hyprland.org/Configuring/Binds/#uncommon-syms--binding-with-a-keycode
-- is to use lower case for keysyms, it it stops working capitalized y'know
bind_exec("XF86AudioNext", "playerctl next")
bind_exec("XF86AudioPrev", "playerctl previous")
bind_exec("XF86AudioPlay", "playerctl play-pause")

-- MPD
bind_exec("SUPER + XF86AudioNext", "playerctl --player=mpd next")
bind_exec("SUPER + XF86AudioPrev", "playerctl --player=mpd previous")
bind_exec("SUPER + XF86AudioPlay", "playerctl --player=mpd play-pause")
bind_exec("SUPER + XF86AudioStop", "playerctl --player=mpd stop; wptoggle")

exec_once("wob-volume-pw")
exec_once("wob-volume-pw-extras")

bind_exec("XF86AudioLowerVolume", "pw-volume @DEFAULT_AUDIO_SINK@ 3-", { repeating = true })
bind_exec("XF86AudioRaiseVolume", "pw-volume @DEFAULT_AUDIO_SINK@ 3+", { repeating = true })

bind_exec("SUPER + ALT + XF86AudioLowerVolume", "env WOB_PIPE=/tmp/wob-game-sink.pipe pw-volume gamesink 5-", { repeating = true })
bind_exec("SUPER + ALT + XF86AudioRaiseVolume", "env WOB_PIPE=/tmp/wob-game-sink.pipe pw-volume gamesink 5+", { repeating = true })
bind_exec("SUPER + XF86AudioLowerVolume", "env WOB_PIPE=/tmp/wob-firefox-sink.pipe pw-volume firefox-sink 5-", { repeating = true })
bind_exec("SUPER + XF86AudioRaiseVolume", "env WOB_PIPE=/tmp/wob-firefox-sink.pipe pw-volume firefox-sink 5+", { repeating = true })
