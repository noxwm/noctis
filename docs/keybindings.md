# Keybindings

The modifier key is **Super** (Win/Logo key).

## Defaults

| Keybind | Action |
|---|---|
| `Super + Enter` | Launch kitty |
| `Super + Q` | Close focused window |
| `Super + J` | Focus next window |
| `Super + K` | Focus previous window |
| `Super + Shift + Q` | Exit compositor |

## Adding Keybinds

In `~/.config/noctis/config.toml`:

```toml
[keybinds]
"Super+Return"  = "exec:kitty"
"Super+B"       = "exec:firefox"
"Super+Shift+F" = "exec:thunar"
"Super+L"       = "exec:swaylock"
"Super+Q"       = "close"
"Super+J"       = "focus_next"
"Super+K"       = "focus_prev"
"Super+Shift+Q" = "exit"
```

## Supported Modifiers

| Config name | Key |
|---|---|
| `Super` | Win / Logo key |
| `Shift` | Shift |
| `Ctrl` | Control |
| `Alt` | Alt |

## Supported Actions

| Action | Description |
|---|---|
| `exec:<cmd>` | Run a command |
| `close` | Close focused window |
| `focus_next` | Focus next window |
| `focus_prev` | Focus previous window |
| `exit` | Exit the compositor |

## Key Names

Key names follow the XKB keysym standard. Common ones:

| Key | Name |
|---|---|
| Enter | `Return` |
| Space | `space` |
| a–z | `a`–`z` |
| A–Z | `A`–`Z` |
| 0–9 | `0`–`9` |
| F1–F12 | `F1`–`F12` |
| Arrow keys | `Left` `Right` `Up` `Down` |
| Backspace | `BackSpace` |
| Escape | `Escape` |
| Tab | `Tab` |

Full list at `/usr/include/xkbcommon/xkbcommon-keysyms.h`
