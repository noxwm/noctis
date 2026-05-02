/*
 * noctis — minimal tiling Wayland compositor
 * Built on the tinywl reference architecture from wlroots.
 *
 * Architecture: single C file, clean wlroots scene-graph approach.
 * Config:       ~/.config/noctis/config.toml (optional, falls back to defaults)
 */

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#include <xkbcommon/xkbcommon.h>
#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/box.h>

/* ── Config ─────────────────────────────────────────────────────────────── */

#define MAX_KEYBINDS 64
#define MAX_AUTOSTART 16

typedef struct {
    uint32_t     modifiers;
    xkb_keysym_t sym;
    char         action[256]; /* "exec:cmd", "close", "focus_next", "focus_prev", "exit" */
} noctis_keybind_t;

typedef struct {
    /* [general] */
    char    terminal[128];
    int     gap;
    float   master_ratio;
    int     border_width;

    /* [colors] RGBA 0-255 stored as float 0.0-1.0 */
    float   bg[4];
    float   active_border[4];
    float   inactive_border[4];

    /* [autostart] */
    char    autostart[MAX_AUTOSTART][256];
    int     autostart_count;

    /* [keybinds] */
    noctis_keybind_t keybinds[MAX_KEYBINDS];
    int     keybind_count;
} noctis_config_t;

static noctis_config_t cfg;

/* Parse "#RRGGBB" or "#RRGGBBAA" into float[4] */
static bool parse_hex_color(const char *hex, float out[4], const char *key) {
    if (!hex || hex[0] != '#') {
        fprintf(stderr, "[noctis] ERROR: %s must start with '#' (got \"%s\")\n",
                key, hex ? hex : "null");
        return false;
    }
    const char *h = hex + 1;
    size_t len = strlen(h);
    if (len != 6 && len != 8) {
        fprintf(stderr, "[noctis] ERROR: %s must be #RRGGBB or #RRGGBBAA\n", key);
        return false;
    }
    unsigned r, g, b, a = 255;
    if (len == 6) sscanf(h, "%02x%02x%02x", &r, &g, &b);
    else          sscanf(h, "%02x%02x%02x%02x", &r, &g, &b, &a);
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
    return true;
}

/* Parse "Super+Shift+Return" → modifiers + sym */
static bool parse_keybind_combo(const char *combo,
                                 uint32_t *mods_out,
                                 xkb_keysym_t *sym_out) {
    char buf[128];
    strncpy(buf, combo, sizeof(buf) - 1);
    buf[sizeof(buf)-1] = '\0';

    *mods_out = 0;
    *sym_out  = XKB_KEY_NoSymbol;

    /* Split by '+' — last token is key, rest are modifiers */
    char *tokens[16];
    int   n = 0;
    char *p = strtok(buf, "+");
    while (p && n < 16) { tokens[n++] = p; p = strtok(NULL, "+"); }
    if (n == 0) return false;

    for (int i = 0; i < n - 1; i++) {
        if (!strcmp(tokens[i], "Super") || !strcmp(tokens[i], "Logo"))
            *mods_out |= WLR_MODIFIER_LOGO;
        else if (!strcmp(tokens[i], "Shift"))
            *mods_out |= WLR_MODIFIER_SHIFT;
        else if (!strcmp(tokens[i], "Ctrl") || !strcmp(tokens[i], "Control"))
            *mods_out |= WLR_MODIFIER_CTRL;
        else if (!strcmp(tokens[i], "Alt"))
            *mods_out |= WLR_MODIFIER_ALT;
        else {
            fprintf(stderr, "[noctis] ERROR: unknown modifier \"%s\" in \"%s\"\n",
                    tokens[i], combo);
            return false;
        }
    }

    *sym_out = xkb_keysym_from_name(tokens[n-1], XKB_KEYSYM_CASE_INSENSITIVE);
    if (*sym_out == XKB_KEY_NoSymbol) {
        fprintf(stderr, "[noctis] ERROR: unknown key \"%s\" in \"%s\"\n",
                tokens[n-1], combo);
        return false;
    }
    return true;
}

static void config_defaults(void) {
    strncpy(cfg.terminal, "kitty", sizeof(cfg.terminal));
    cfg.gap          = 6;
    cfg.master_ratio = 0.55f;
    cfg.border_width = 2;

    cfg.bg[0] = 0.10f; cfg.bg[1] = 0.10f; cfg.bg[2] = 0.15f; cfg.bg[3] = 1.0f;
    cfg.active_border[0]   = 0.67f; cfg.active_border[1]   = 0.42f;
    cfg.active_border[2]   = 0.42f; cfg.active_border[3]   = 1.0f;
    cfg.inactive_border[0] = 0.20f; cfg.inactive_border[1] = 0.20f;
    cfg.inactive_border[2] = 0.20f; cfg.inactive_border[3] = 1.0f;

    cfg.autostart_count = 0;
    cfg.keybind_count   = 0;

    /* Default keybinds */
    struct { const char *combo; const char *action; } defaults[] = {
        { "Super+Return",  "exec:kitty"  },
        { "Super+Q",       "close"       },
        { "Super+J",       "focus_next"  },
        { "Super+K",       "focus_prev"  },
        { "Super+Shift+Q", "exit"        },
    };
    int nd = sizeof(defaults) / sizeof(defaults[0]);
    for (int i = 0; i < nd; i++) {
        noctis_keybind_t *kb = &cfg.keybinds[cfg.keybind_count];
        if (parse_keybind_combo(defaults[i].combo,
                                 &kb->modifiers, &kb->sym)) {
            strncpy(kb->action, defaults[i].action, sizeof(kb->action));
            cfg.keybind_count++;
        }
    }
}

/* ── Minimal TOML parser (hand-rolled, no deps) ─────────────────────────── */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n'))
        *e-- = '\0';
    return s;
}

static void config_load(void) {
    config_defaults();

    const char *xdg = getenv("XDG_CONFIG_HOME");
    char path[512];
    if (xdg && *xdg)
        snprintf(path, sizeof(path), "%s/noctis/config.toml", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home) { wlr_log(WLR_INFO, "HOME not set, using defaults"); return; }
        snprintf(path, sizeof(path), "%s/.config/noctis/config.toml", home);
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        wlr_log(WLR_INFO, "No config at %s — using defaults", path);
        return;
    }
    wlr_log(WLR_INFO, "Loading config: %s", path);

    char line[512];
    char section[64] = "";
    /* When [keybinds] section found, clear defaults */
    bool keybinds_section_seen = false;

    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (*l == '#' || *l == '\0') continue;

        /* Section header */
        if (*l == '[') {
            char *end = strchr(l, ']');
            if (end) {
                *end = '\0';
                strncpy(section, l + 1, sizeof(section) - 1);
                if (!strcmp(section, "keybinds") && !keybinds_section_seen) {
                    cfg.keybind_count = 0;
                    keybinds_section_seen = true;
                }
                if (!strcmp(section, "autostart")) {
                    cfg.autostart_count = 0;
                }
            }
            continue;
        }

        /* key = value */
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(l);
        char *val = trim(eq + 1);

        /* Strip quotes from value */
        if (*val == '"') {
            val++;
            char *qe = strrchr(val, '"');
            if (qe) *qe = '\0';
        }

        if (!strcmp(section, "general")) {
            if (!strcmp(key, "terminal"))
                strncpy(cfg.terminal, val, sizeof(cfg.terminal));
            else if (!strcmp(key, "gap")) {
                int v = atoi(val);
                if (v < 0 || v > 200) {
                    fprintf(stderr, "[noctis] ERROR: gap must be 0-200 (got %d)\n", v);
                    exit(1);
                }
                cfg.gap = v;
            }
            else if (!strcmp(key, "master_ratio")) {
                float v = atof(val);
                if (v <= 0.0f || v >= 1.0f) {
                    fprintf(stderr, "[noctis] ERROR: master_ratio must be 0.0-1.0 (got %.3f)\n", v);
                    exit(1);
                }
                cfg.master_ratio = v;
            }
            else if (!strcmp(key, "border_width")) {
                int v = atoi(val);
                if (v < 0 || v > 50) {
                    fprintf(stderr, "[noctis] ERROR: border_width must be 0-50 (got %d)\n", v);
                    exit(1);
                }
                cfg.border_width = v;
            }
        }
        else if (!strcmp(section, "colors")) {
            float tmp[4];
            if (!strcmp(key, "background")) {
                if (!parse_hex_color(val, tmp, "colors.background")) exit(1);
                memcpy(cfg.bg, tmp, sizeof(tmp));
            } else if (!strcmp(key, "active_border")) {
                if (!parse_hex_color(val, tmp, "colors.active_border")) exit(1);
                memcpy(cfg.active_border, tmp, sizeof(tmp));
            } else if (!strcmp(key, "inactive_border")) {
                if (!parse_hex_color(val, tmp, "colors.inactive_border")) exit(1);
                memcpy(cfg.inactive_border, tmp, sizeof(tmp));
            }
        }
        else if (!strcmp(section, "autostart")) {
            /* apps = ["waybar", "dunst"] — parse array */
            if (!strcmp(key, "apps")) {
                /* val should look like: ["waybar", "dunst"] */
                char *p = val;
                while (*p) {
                    char *qs = strchr(p, '"');
                    if (!qs) break;
                    char *qe = strchr(qs + 1, '"');
                    if (!qe) break;
                    *qe = '\0';
                    if (cfg.autostart_count < MAX_AUTOSTART) {
                        strncpy(cfg.autostart[cfg.autostart_count++],
                                qs + 1, 255);
                    }
                    p = qe + 1;
                }
            }
        }
        else if (!strcmp(section, "keybinds")) {
            if (cfg.keybind_count < MAX_KEYBINDS) {
                noctis_keybind_t *kb = &cfg.keybinds[cfg.keybind_count];
                /* key is the combo (may have quotes stripped already) */
                if (parse_keybind_combo(key, &kb->modifiers, &kb->sym)) {
                    strncpy(kb->action, val, sizeof(kb->action));
                    cfg.keybind_count++;
                } else {
                    exit(1);
                }
            }
        }
    }

    fclose(f);
    wlr_log(WLR_INFO, "Config loaded — terminal=%s gap=%d keybinds=%d autostart=%d",
            cfg.terminal, cfg.gap, cfg.keybind_count, cfg.autostart_count);
}

/* ── Compositor structs ──────────────────────────────────────────────────── */

struct noctis_server {
    struct wl_display          *display;
    struct wlr_backend         *backend;
    struct wlr_renderer        *renderer;
    struct wlr_allocator       *allocator;
    struct wlr_scene           *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_xdg_shell       *xdg_shell;
    struct wl_listener          new_xdg_surface;

    struct wlr_cursor          *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener          cursor_motion;
    struct wl_listener          cursor_motion_absolute;
    struct wl_listener          cursor_button;
    struct wl_listener          cursor_axis;
    struct wl_listener          cursor_frame;

    struct wlr_seat            *seat;
    struct wl_listener          request_cursor;
    struct wl_listener          request_set_selection;

    struct wlr_output_layout   *output_layout;
    struct wl_list              outputs;
    struct wl_listener          new_output;
    struct wl_listener          new_input;

    struct wl_list              views;
    struct noctis_view         *focused_view;
};

struct noctis_output {
    struct wl_list              link;
    struct noctis_server       *server;
    struct wlr_output          *wlr_output;
    struct wlr_scene_output    *scene_output;
    struct wl_listener          frame;
    struct wl_listener          request_state;
    struct wl_listener          destroy;
};

struct noctis_view {
    struct wl_list              link;
    struct noctis_server       *server;
    struct wlr_xdg_toplevel    *toplevel;
    struct wlr_scene_tree      *scene_tree;
    bool                        mapped;
    int x, y, w, h;
    struct wl_listener          map;
    struct wl_listener          unmap;
    struct wl_listener          destroy;
    struct wl_listener          request_move;
    struct wl_listener          request_resize;
    struct wl_listener          request_maximize;
    struct wl_listener          request_fullscreen;
};

struct noctis_keyboard {
    struct wl_list              link;
    struct noctis_server       *server;
    struct wlr_keyboard        *wlr_keyboard;
    struct wl_listener          modifiers;
    struct wl_listener          key;
    struct wl_listener          destroy;
};

/* ── Spawn ───────────────────────────────────────────────────────────────── */

static void spawn(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        pid_t inner = fork();
        if (inner == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", cmd, NULL);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

/* ── Layout ──────────────────────────────────────────────────────────────── */

static void apply_layout(struct noctis_server *server) {
    struct wlr_box box = {0};
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    if (box.width == 0 || box.height == 0) return;

    /* Collect mapped views */
    struct noctis_view *views[256];
    int n = 0;
    struct noctis_view *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped) views[n++] = v;
        if (n >= 256) break;
    }
    if (n == 0) return;

    int g  = cfg.gap;
    int ax = box.x + g;
    int ay = box.y + g;
    int aw = box.width  - 2 * g;
    int ah = box.height - 2 * g;

    if (n == 1) {
        wlr_scene_node_set_position(&views[0]->scene_tree->node, ax, ay);
        wlr_xdg_toplevel_set_size(views[0]->toplevel,
            (uint32_t)(aw > 0 ? aw : 1),
            (uint32_t)(ah > 0 ? ah : 1));
        views[0]->x = ax; views[0]->y = ay;
        views[0]->w = aw; views[0]->h = ah;
        return;
    }

    int mw = (int)(aw * cfg.master_ratio) - g / 2;
    int sx = ax + mw + g;
    int sw = aw - mw - g;

    wlr_scene_node_set_position(&views[0]->scene_tree->node, ax, ay);
    wlr_xdg_toplevel_set_size(views[0]->toplevel,
        (uint32_t)(mw > 0 ? mw : 1),
        (uint32_t)(ah > 0 ? ah : 1));
    views[0]->x = ax; views[0]->y = ay;
    views[0]->w = mw; views[0]->h = ah;

    int ns    = n - 1;
    int each  = (ah - (ns - 1) * g) / ns;
    for (int i = 0; i < ns; i++) {
        int sy = ay + i * (each + g);
        wlr_scene_node_set_position(&views[i+1]->scene_tree->node, sx, sy);
        wlr_xdg_toplevel_set_size(views[i+1]->toplevel,
            (uint32_t)(sw > 0 ? sw : 1),
            (uint32_t)(each > 0 ? each : 1));
        views[i+1]->x = sx; views[i+1]->y = sy;
        views[i+1]->w = sw; views[i+1]->h = each;
    }
}

/* ── Focus ───────────────────────────────────────────────────────────────── */

static void focus_view(struct noctis_server *server, struct noctis_view *view) {
    if (!view || !view->mapped) return;

    struct noctis_view *prev = server->focused_view;
    if (prev == view) return;

    if (prev) wlr_xdg_toplevel_set_activated(prev->toplevel, false);

    server->focused_view = view;
    wlr_xdg_toplevel_set_activated(view->toplevel, true);
    wlr_scene_node_raise_to_top(&view->scene_tree->node);

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    if (kb) {
        wlr_seat_keyboard_notify_enter(server->seat,
            view->toplevel->base->surface,
            kb->keycodes, kb->num_keycodes,
            &kb->modifiers);
    }
}

static void focus_next(struct noctis_server *server) {
    struct noctis_view *views[256];
    int n = 0;
    struct noctis_view *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped) views[n++] = v;
        if (n >= 256) break;
    }
    if (n == 0) return;
    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (views[i] == server->focused_view) {
            idx = (i + 1) % n;
            break;
        }
    }
    focus_view(server, views[idx]);
}

static void focus_prev(struct noctis_server *server) {
    struct noctis_view *views[256];
    int n = 0;
    struct noctis_view *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped) views[n++] = v;
        if (n >= 256) break;
    }
    if (n == 0) return;
    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (views[i] == server->focused_view) {
            idx = (i == 0) ? n - 1 : i - 1;
            break;
        }
    }
    focus_view(server, views[idx]);
}

/* ── Surface hit-test ────────────────────────────────────────────────────── */

static struct wlr_surface *desktop_surface_at(
        struct noctis_server *server,
        double lx, double ly,
        double *sx, double *sy) {
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
    struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *ssurf = wlr_scene_surface_try_from_buffer(sbuf);
    if (!ssurf) return NULL;
    return ssurf->surface;
}

/* ── Keyboard ────────────────────────────────────────────────────────────── */

static bool handle_keybind(struct noctis_server *server,
                             uint32_t mods, xkb_keysym_t sym) {
    for (int i = 0; i < cfg.keybind_count; i++) {
        noctis_keybind_t *kb = &cfg.keybinds[i];
        if (kb->modifiers != mods || kb->sym != sym) continue;

        if (!strncmp(kb->action, "exec:", 5)) {
            spawn(kb->action + 5);
        } else if (!strcmp(kb->action, "close")) {
            if (server->focused_view)
                wlr_xdg_toplevel_send_close(server->focused_view->toplevel);
        } else if (!strcmp(kb->action, "focus_next")) {
            focus_next(server);
        } else if (!strcmp(kb->action, "focus_prev")) {
            focus_prev(server);
        } else if (!strcmp(kb->action, "exit")) {
            wlr_log(WLR_INFO, "Exiting noctis");
            wl_display_terminate(server->display);
        }
        return true;
    }
    return false;
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    struct noctis_keyboard *kb =
        wl_container_of(listener, kb, modifiers);
    wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(kb->server->seat,
        &kb->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct noctis_keyboard *kb =
        wl_container_of(listener, kb, key);
    struct noctis_server *server = kb->server;
    struct wlr_keyboard_key_event *ev = data;

    uint32_t keycode = ev->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(
        kb->wlr_keyboard->xkb_state, keycode, &syms);

    uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

    bool handled = false;
    if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms && !handled; i++)
            handled = handle_keybind(server, mods, syms[i]);
    }

    if (!handled) {
        wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat,
            ev->time_msec, ev->keycode, ev->state);
    }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    struct noctis_keyboard *kb =
        wl_container_of(listener, kb, destroy);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->destroy.link);
    wl_list_remove(&kb->link);
    free(kb);
}

static void new_keyboard(struct noctis_server *server,
                          struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(device);

    struct noctis_keyboard *kb = calloc(1, sizeof(*kb));
    kb->server       = server;
    kb->wlr_keyboard = wlr_kb;

    struct xkb_context *ctx    = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap  *keymap = xkb_keymap_new_from_names(
        ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_kb, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(wlr_kb, 25, 600);

    kb->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_kb->events.modifiers, &kb->modifiers);

    kb->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_kb->events.key, &kb->key);

    kb->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &kb->destroy);

    wlr_seat_set_keyboard(server->seat, wlr_kb);
    wlr_log(WLR_INFO, "New keyboard: %s", device->name);
}

static void new_pointer(struct noctis_server *server,
                         struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
    wlr_log(WLR_INFO, "New pointer: %s", device->name);
}

/* ── Input handler ───────────────────────────────────────────────────────── */

static void server_new_input(struct wl_listener *listener, void *data) {
    struct noctis_server *server =
        wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: new_keyboard(server, device); break;
    case WLR_INPUT_DEVICE_POINTER:  new_pointer(server, device);  break;
    default: break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

/* ── Cursor ──────────────────────────────────────────────────────────────── */

static void process_cursor_motion(struct noctis_server *server,
                                   uint32_t time_msec) {
    double sx, sy;
    struct wlr_surface *surface =
        desktop_surface_at(server, server->cursor->x, server->cursor->y,
                           &sx, &sy);
    if (!surface) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
        wlr_seat_pointer_clear_focus(server->seat);
    } else {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
    }
}

static void cursor_motion(struct wl_listener *listener, void *data) {
    struct noctis_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *ev = data;
    wlr_cursor_move(server->cursor, &ev->pointer->base,
                    ev->delta_x, ev->delta_y);
    process_cursor_motion(server, ev->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct noctis_server *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *ev = data;
    wlr_cursor_warp_absolute(server->cursor, &ev->pointer->base, ev->x, ev->y);
    process_cursor_motion(server, ev->time_msec);
}

static void cursor_button(struct wl_listener *listener, void *data) {
    struct noctis_server *server =
        wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *ev = data;
    wlr_seat_pointer_notify_button(server->seat,
        ev->time_msec, ev->button, ev->state);

    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        double sx, sy;
        struct wlr_surface *surface =
            desktop_surface_at(server, server->cursor->x, server->cursor->y,
                               &sx, &sy);
        if (surface) {
            struct wlr_xdg_surface *xdg =
                wlr_xdg_surface_try_from_wlr_surface(surface);
            if (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
                struct noctis_view *v;
                wl_list_for_each(v, &server->views, link) {
                    if (v->toplevel == xdg->toplevel && v->mapped) {
                        focus_view(server, v);
                        break;
                    }
                }
            }
        }
    }
}

static void cursor_axis(struct wl_listener *listener, void *data) {
    struct noctis_server *server =
        wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *ev = data;
    wlr_seat_pointer_notify_axis(server->seat, ev->time_msec,
        ev->orientation, ev->delta, ev->delta_discrete, ev->source
#if defined(WLR_018)
        , ev->relative_direction
#endif
        );
}

static void cursor_frame(struct wl_listener *listener, void *data) {
    struct noctis_server *server =
        wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

/* ── Seat ────────────────────────────────────────────────────────────────── */

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct noctis_server *server =
        wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *ev = data;
    if (ev->seat_client == server->seat->pointer_state.focused_client)
        wlr_cursor_set_surface(server->cursor, ev->surface,
                               ev->hotspot_x, ev->hotspot_y);
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    struct noctis_server *server =
        wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *ev = data;
    wlr_seat_set_selection(server->seat, ev->source, ev->serial);
}

/* ── Views ───────────────────────────────────────────────────────────────── */

static void view_map(struct wl_listener *listener, void *data) {
    struct noctis_view *view = wl_container_of(listener, view, map);
    view->mapped = true;
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    focus_view(view->server, view);
    apply_layout(view->server);
}

static void view_unmap(struct wl_listener *listener, void *data) {
    struct noctis_view *view = wl_container_of(listener, view, unmap);
    view->mapped = false;

    if (view->server->focused_view == view) {
        view->server->focused_view = NULL;
        struct noctis_view *v;
        wl_list_for_each(v, &view->server->views, link) {
            if (v != view && v->mapped) {
                focus_view(view->server, v);
                break;
            }
        }
    }
    apply_layout(view->server);
}

static void view_destroy(struct wl_listener *listener, void *data) {
    struct noctis_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->link);
    free(view);
}

static void view_request_move(struct wl_listener *l, void *d) { /* tiling — ignore */ }
static void view_request_resize(struct wl_listener *l, void *d) { /* tiling — ignore */ }

static void view_request_maximize(struct wl_listener *listener, void *data) {
    struct noctis_view *v = wl_container_of(listener, v, request_maximize);
    wlr_xdg_toplevel_set_maximized(v->toplevel, false);
    wlr_xdg_surface_schedule_configure(v->toplevel->base);
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) {
    struct noctis_view *v = wl_container_of(listener, v, request_fullscreen);
    wlr_xdg_toplevel_set_fullscreen(v->toplevel, false);
    wlr_xdg_surface_schedule_configure(v->toplevel->base);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct noctis_server *server =
        wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;

    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;

    struct noctis_view *view = calloc(1, sizeof(*view));
    view->server     = server;
    view->toplevel   = xdg_surface->toplevel;
    view->scene_tree = wlr_scene_xdg_surface_create(
        &server->scene->tree, xdg_surface);
    view->mapped = false;

    view->map.notify = view_map;
    wl_signal_add(&xdg_surface->surface->events.map, &view->map);

    view->unmap.notify = view_unmap;
    wl_signal_add(&xdg_surface->surface->events.unmap, &view->unmap);

    view->destroy.notify = view_destroy;
#if defined(WLR_018)
    wl_signal_add(&xdg_surface->toplevel->events.destroy, &view->destroy);
#else
    wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
#endif

    view->request_move.notify = view_request_move;
    wl_signal_add(&xdg_surface->toplevel->events.request_move,
                  &view->request_move);

    view->request_resize.notify = view_request_resize;
    wl_signal_add(&xdg_surface->toplevel->events.request_resize,
                  &view->request_resize);

    view->request_maximize.notify = view_request_maximize;
    wl_signal_add(&xdg_surface->toplevel->events.request_maximize,
                  &view->request_maximize);

    view->request_fullscreen.notify = view_request_fullscreen;
    wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen,
                  &view->request_fullscreen);

    wl_list_insert(&server->views, &view->link);
    wlr_log(WLR_INFO, "New toplevel: %s",
            xdg_surface->toplevel->app_id ?
            xdg_surface->toplevel->app_id : "(unknown)");
}

/* ── Output ──────────────────────────────────────────────────────────────── */

static void output_frame(struct wl_listener *listener, void *data) {
    struct noctis_output *output =
        wl_container_of(listener, output, frame);
    wlr_scene_output_commit(output->scene_output, NULL);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
    struct noctis_output *output =
        wl_container_of(listener, output, request_state);
    const struct wlr_output_state *state = data;
    wlr_output_commit_state(output->wlr_output, state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    struct noctis_output *output =
        wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct noctis_server *server =
        wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) wlr_output_state_set_mode(&state, mode);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct noctis_output *output = calloc(1, sizeof(*output));
    output->server     = server;
    output->wlr_output = wlr_output;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    struct wlr_output_layout_output *l_out =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);

    output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_out,
                                        output->scene_output);

    wl_list_insert(&server->outputs, &output->link);

    wlr_log(WLR_INFO, "New output: %s (%dx%d)",
            wlr_output->name, wlr_output->width, wlr_output->height);

    apply_layout(server);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* Ignore SIGCHLD to avoid zombies */
    signal(SIGCHLD, SIG_IGN);

    wlr_log_init(WLR_DEBUG, NULL);
    config_load();

    struct noctis_server server = {0};
    wl_list_init(&server.outputs);
    wl_list_init(&server.views);

    /* Display */
    server.display = wl_display_create();

    /* Backend — wlroots 0.18 takes wl_display*, older also takes wl_display* */
    server.backend = wlr_backend_autocreate(server.display, NULL);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "Failed to create backend");
        return 1;
    }

    /* Renderer & allocator */
    server.renderer = wlr_renderer_autocreate(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.display);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

    /* Wayland globals */
    wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    /* Output layout */
#if defined(WLR_018)
    server.output_layout = wlr_output_layout_create(server.display);
#else
    server.output_layout = wlr_output_layout_create();
#endif

    /* Scene graph */
    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(
        server.scene, server.output_layout);

    /* XDG shell */
    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_surface.notify = server_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface,
                  &server.new_xdg_surface);

    /* Cursor */
    server.cursor     = wlr_cursor_create();
    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    wlr_xcursor_manager_load(server.cursor_mgr, 1);

    server.cursor_motion.notify = cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute,
                  &server.cursor_motion_absolute);
    server.cursor_button.notify = cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    /* Seat */
    server.seat = wlr_seat_create(server.display, "seat0");
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor,
                  &server.request_cursor);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection,
                  &server.request_set_selection);

    /* Backend events */
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    /* Socket */
    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Failed to create socket");
        return 1;
    }

    /* Start backend */
    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend");
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    wlr_log(WLR_INFO, "noctis running on WAYLAND_DISPLAY=%s", socket);
    wlr_log(WLR_INFO, "Keybinds loaded: %d", cfg.keybind_count);

    /* Autostart */
    for (int i = 0; i < cfg.autostart_count; i++) {
        wlr_log(WLR_INFO, "Autostart: %s", cfg.autostart[i]);
        spawn(cfg.autostart[i]);
    }

    /* Event loop */
    wl_display_run(server.display);

    /* Cleanup */
    wl_display_destroy_clients(server.display);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);

    return 0;
}
