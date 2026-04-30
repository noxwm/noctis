<div align="center">


# noctis

*A **Cxtremely** minimal tiling Wayland compositor.*

<br/>


</div>

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
