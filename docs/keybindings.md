# noctis keybindings

Keybinds are defined in `~/.config/noctis/config.nox`.

## Format

```
bind = MODS, Key, action[, args]
```

Modifiers: `SUPER`, `SHIFT`, `CTRL`, `ALT` — space separated for combos.

## Actions

| Action       | Description                  |
|--------------|------------------------------|
| `exec, cmd`  | Run a command                |
| `close`      | Close focused window         |
| `focus_next` | Focus next window            |
| `focus_prev` | Focus previous window        |
| `exit`       | Exit the compositor          |

## Examples

```
bind = SUPER, Return, exec, kitty
bind = SUPER, B, exec, firefox
bind = SUPER SHIFT, F, exec, thunar
bind = SUPER, Q, close
bind = SUPER, J, focus_next
bind = SUPER, K, focus_prev
bind = SUPER SHIFT, Q, exit
bind = SUPER, L, exec, swaylock
```
