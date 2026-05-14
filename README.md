# noxwm

A Wayland compositor with niri-style scrollable column tiling and Hyprland-style workspaces.

Built with C++23 and wlroots.

## Architecture

```
src/
  main.cpp                 — entry point
  core/
    server.cpp             — compositor core, event loop, wlroots wiring
    output.cpp             — display output handling (DRM/KMS)
    xdg_shell.cpp          — reserved for future protocol extensions
  input/
    keyboard.cpp           — keyboard input + keybind dispatch
    cursor.cpp             — reserved for cursor theme work
  layout/
    column_layout.cpp      — niri-style scrollable column tiling engine
    workspace.cpp          — workspace management
  config/
    parser.cpp             — .nox config lexer + parser
    config.cpp             — config loader
```

## Building

### Dependencies

- wlroots >= 0.18
- wayland-server
- xkbcommon
- pixman
- libinput
- meson + ninja

```bash
# Arch Linux
sudo pacman -S wlroots wayland xkbcommon pixman libinput meson ninja

# Ubuntu/Debian
sudo apt install libwlroots-dev libwayland-dev libxkbcommon-dev \
                 libpixman-1-dev libinput-dev meson ninja-build
```

### Compile

```bash
meson setup build
ninja -C build
```

### Run

```bash
# From a TTY (will launch on DRM/KMS)
./build/noxwm

# From inside an existing Wayland session (for testing)
./build/noxwm  # wlroots auto-detects and uses a nested backend
```

## Config

Copy the example config:

```bash
mkdir -p ~/.config/noxwm
cp config.nox.example ~/.config/noxwm/config.nox
```

Edit `~/.config/noxwm/config.nox`:

```
general {
    gaps = 8
    border_width = 2
    border_color = #cba6f7
    border_color_inactive = #313244
}

keybinds {
    Super+Return = exec kitty
    Super+Q = close
    Super+H = focus left
    Super+L = focus right
    Super+1 = workspace 1
}

autostart {
    exec waybar
}
```

## Layout model

Each workspace uses a scrollable column layout:

```
Workspace 1:  [ terminal ] [ browser ] [ editor ] →  scroll with Super+H/L
Workspace 2:  [ discord ]  [ spotify ]
```

- Every new window opens in its own column
- Windows in the same column stack vertically
- Columns scroll horizontally, keeping the focused column visible
- Switch workspaces with Super+1 through Super+9

## v1 scope

- [x] Tiling (scrollable columns)
- [x] Named workspaces (9 total)
- [x] Focus with keyboard (Super+H/L) and mouse click
- [x] .nox config parser
- [x] Keybind dispatch
- [x] Autostart programs
- [x] Close window / fullscreen
- [ ] Border rendering (v1.1)
- [ ] Multi-monitor (v2)
- [ ] XWayland (v2)
- [ ] Animations (v2)
