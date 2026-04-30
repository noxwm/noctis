<div align="center">

<img src="https://raw.githubusercontent.com/noxwm/noctis/main/docs/logo.png" width="180"/>

# noctis

*A **Cxtremely** minimal tiling Wayland compositor.*

<br/>

[![GitHub Stars](https://img.shields.io/github/stars/noxwm/noctis?style=for-the-badge&logo=github&logoColor=D9E0EE&labelColor=252733&color=AB6C6A)](https://github.com/noxwm/noctis/stargazers)
[![License](https://img.shields.io/badge/license-MIT-blue?style=for-the-badge&logoColor=D9E0EE&labelColor=252733&color=AB6C6A)](LICENSE)
[![Built With](https://img.shields.io/badge/built%20with-wlroots%200.17-orange?style=for-the-badge&logoColor=D9E0EE&labelColor=252733&color=AB6C6A)](https://gitlab.freedesktop.org/wlroots/wlroots)

</div>

---

<div align="center">

📸 Screenshots

</div>

---

> screenshots go here

---

## ⚡ Install (Arch Linux)

```bash
sudo pacman -S cmake pkgconf wlroots wayland wayland-protocols \
               libxkbcommon libinput pixman tomlplusplus
```

```bash
git clone https://github.com/noxwm/noctis
cd noctis
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/noctis
```

---

## 🎨 Configuration

Copy the default config to get started:

```bash
mkdir -p ~/.config/noctis
cp config/config.toml ~/.config/noctis/config.toml
```

Then edit `~/.config/noctis/config.toml` — changes apply on next start.

```toml
[general]
terminal     = "kitty"
gap          = 6
master_ratio = 0.55
border_width = 2

[colors]
background      = "#1a1a26"
active_border   = "#AB6C6A"
inactive_border = "#333333"

[autostart]
apps = ["waybar", "dunst"]

[keybinds]
"Super+Return"  = "exec:kitty"
"Super+Q"       = "close"
"Super+J"       = "focus_next"
"Super+K"       = "focus_prev"
"Super+Shift+Q" = "exit"
```

---

## 🖼 Themes

Ready-made themes are in the `examples/` folder:

| File | Theme |
|:---|:---|
| `noxwm-default.toml` | noxwm rose (default) |
| `catppuccin-mocha.toml` | Catppuccin Mocha |
| `nord.toml` | Nord |
| `gruvbox.toml` | Gruvbox Dark |
| `rose-pine.toml` | Rosé Pine |
| `minimal-no-gaps.toml` | Minimal, no gaps, no borders |

Apply one:
```bash
cp examples/catppuccin-mocha.toml ~/.config/noctis/config.toml
```

---

## ⌨️ Keybindings

| Keybind | Action |
|:---|:---|
| `Super + Enter` | Launch kitty |
| `Super + Q` | Close window |
| `Super + J` | Focus next |
| `Super + K` | Focus previous |
| `Super + Shift + Q` | Exit |

See [`docs/keybindings.md`](docs/keybindings.md) for full reference.

---

## 🗂 Structure

```
src/          → compositor source
protocols/    → xdg-shell generated headers
config/       → default config.toml
examples/     → ready-made themes
scripts/      → build helpers
docs/         → keybindings reference
```

---

## 🔗 Related

- [noctctl](https://github.com/noxwm/noctctl) — IPC client for noctis

---

<div align="center">

made with 🖤 on Arch Linux

</div>
