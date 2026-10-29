# Snowhush for Hyprland

Snow falls on your desktop, settles along the bottom of the screen and on the
top edges of windows, slumps at its angle of repose, parts under the pointer,
slides off windows you shake, and bursts into powder when a window closes.

A port of [mbezruchko/Snowhush](https://github.com/mbezruchko/Snowhush) (MIT,
macOS) to Wayland. The simulation is a line-for-line port of the original Swift;
the window and pointer data that CoreGraphics provided on macOS come from
Hyprland's IPC instead, and the Metal renderer became GLSL ES 3.0 on a
`wlr-layer-shell` surface.

## Build

Dependencies: `wayland`, `mesa` (EGL, GLES2/3), `libpng` (optional, for the
wallpaper tint), `libsystemd` (for the tray icon) and `wayland-protocols`
(xdg-shell is needed at link time).

```sh
make
make install          # ~/.local/bin/snowhush
```

`make` generates the layer-shell and xdg-shell protocol code into `build/`.

## Run

```sh
snowhush              # snow on the overlay layer, in front of windows
snowhush --behind     # behind windows instead of in front
snowhush --snowfall heavy --fps 120
```

The surfaces take no input, so clicks go straight through to the window below.

A systemd user unit:

```ini
[Unit]
Description=Snowhush snow
After=graphical-session.target

[Service]
ExecStart=%h/.local/bin/snowhush
Restart=on-failure

[Install]
WantedBy=graphical-session.target
```

## Tray menu

The snowflake in the tray (Waybar's `tray` module, ideally right where you want
it) opens a menu. Every switch shows its state in the label itself — `✓` for an
enabled option, a blank of the same width for a disabled one, `●`/`○` for the
snowfall choice — because Waybar draws no checkbox of its own:

| Entry | What it does |
|---|---|
| Snowfall → ○ Off / ○ Light / ● Steady / ○ Heavy / ○ Variable | how hard it snows; `Variable` ramps the strength up and down over a few minutes instead of holding one rate |
| Screens → ✓ HDMI-A-1 / ✓ HDMI-A-2 | snow on that output, or not; untick one and its overlay is blanked and its simulation frozen (it picks up where it left off when ticked again) |
| ✓ Snow on windows | snow collects on the top edges of windows; off means a level field, snow only on the bottom of the screen |
| ✓ Accumulate | snow settles into drifts instead of being swept away as it lands |
| ✓ Melt | drifts lose a little height over time; off keeps them forever |
| ✓ No snow over fullscreen | while a window on that output is fullscreen the overlay is hidden entirely, so a film or a game gets a clean screen |
| Clear snow | a gust blows every flake and drift off the screen |
| Quit Snowhush | stop the app |

With `ItemIsMenu` set the host sends a left click to the menu, so the icon
always opens it; a host that still sends `Activate` toggles the snowfall
between `steady` and `off`.

The menu is a `com.canonical.dbusmenu` service next to the
`StatusNotifierItem`, so no GTK is involved: any StatusNotifierHost shows it,
and the app is a plain Wayland client.

## Command line

Every menu entry is reachable from a socket, which is handy for keybinds:

```sh
snowhush ctl status
snowhush ctl snowfall off|light|steady|heavy|variable
snowhush ctl accumulate on|off
snowhush ctl melt on|off
snowhush ctl windows on|off
snowhush ctl behind on|off
snowhush ctl fullscreen on|off      # no snow over a fullscreen window
snowhush ctl monitors               # per-output state
snowhush ctl monitor HDMI-A-2 off   # snow only on the other output
snowhush ctl monitors all           # back on everywhere
snowhush ctl clear
snowhush ctl surfaces               # what the tracker sees, for debugging
snowhush ctl quit
```

`status`, `monitors` and the tray menu all read the same settings, and every
change is written to the config file.

## Config

`~/.config/snowhush/config`, rewritten whenever a setting changes:

```ini
snowfall = steady              # off | light | steady | heavy | variable
accumulate = on
melt = on
snow_on_windows = on
behind_windows = off
monitors_off =                 # comma separated outputs without snow
hide_fullscreen = on
fps = 60                       # ceiling for the interactive frame rate
```

## How it works

| macOS | here |
|---|---|
| `CGWindowListCopyWindowInfo` | `hyprctl -j clients` over the `.socket.sock` IPC |
| `NSScreen` list | `hyprctl -j monitors` |
| menu bar height | top layer-shell band (Waybar), `hyprctl -j layers` |
| `NSEvent.mouseLocation` | `hyprctl cursorpos` |
| `NSWindow` overlay | `zwlr_layer_shell_v1` surface per output, EGL + GLES3 |
| Metal point sprites | instanced quads, one per flake, with the same shading |
| Dock strip | not needed: the snow goes to the bottom of the output |

Window coordinates are flipped into the simulator's bottom-left origin, and
window corner radius comes from Hyprland's `decoration:rounding`, so the snow
follows the actual rounded top edge. Wide, shallow windows (search bars) are
treated as capsules, as in the original.

A window that fills the working area carries no snow: there is no sky above it
and its top edge sits at the screen edge. That matches the reference.

The overlay is hidden while a window on that output is fullscreen (Hyprland
reports `fullscreen: 2`), and on outputs switched off in the menu or config.

## Files

```
src/sim.c      the simulation: flake, grain and layer physics
src/gl.c       GLSL ES shaders and the instanced renderer
src/wl.c       layer-shell surfaces, EGL, frame callbacks
src/hypr.c     Hyprland IPC: clients, monitors, layers, pointer
src/tray.c     StatusNotifierItem + dbusmenu
src/main.c     tracker, frame loop, config, control socket, menu model
protocol/      vendored wlr-layer-shell XML
tests/         menu_probe.c: checks the tray menu with libdbusmenu
```

`tests/menu_probe.c` is a small libdbusmenu client used to confirm that Waybar
can parse the menu:

```sh
cc tests/menu_probe.c -o build/menu_probe $(pkg-config --cflags --libs gtk+-3.0 libdbusmenu-glib-0.4)
./build/menu_probe org.kde.StatusNotifierItem-$(pidof snowhush)-1 /MenuBar
```

## Not ported

* Dock snow: there is no dock, the bottom of the output is the ground.
* "Show in Screenshots": every Wayland screenshot tool sees the overlay anyway.
* macOS Spaces bookkeeping: Hyprland workspaces replace it; snow on a window
  that leaves the workspace waits for it and comes back with it.
# snow-hyprland
# snow-hyprland
