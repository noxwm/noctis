# wayland-compositor

A minimal, functional Wayland compositor with master-stack tiling, built on wlroots 0.17 in C++17.

Repository: https://github.com/notcandy001/wayland-compositor

---

## Features

- wlroots scene-graph rendering
- xdg-shell surface management
- Master-stack tiling
- Keyboard focus cycling (Super+J/K)
- Click-to-focus
- XKB keymap support
- libinput support
- Multi-output support
- Keybindings system

---

## Dependencies (Arch Linux)

```bash
sudo pacman -S --needed \
  base-devel cmake pkgconf \
  wlroots wayland wayland-protocols \
  libxkbcommon libinput pixman
```

Optional:

```bash
sudo pacman -S alacritty
```

---

## Build

```bash
git clone https://github.com/notcandy001/wayland-compositor
cd wayland-compositor

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

---

## Run

```bash
./build/compositor
```

---

## Keybindings

| Keybind | Action |
|--------|--------|
| Super + Enter | Launch terminal (alacritty) |
| Super + Q | Close focused window |
| Super + J / K | Cycle focus |
| Super + Shift + Q | Exit compositor |

---

## Configuration

Edit `src/config.cpp`:

```cpp
terminal     = "alacritty";
border_width = 2;
gap          = 6;
master_ratio = 0.55f;
bg_color     = {0.10, 0.10, 0.15, 1.0};
```

---

## Notes

- Arch Linux only
- Uses wlroots backend
- Minimal tiling compositor (master-stack layout)

---

## License

AGPL
