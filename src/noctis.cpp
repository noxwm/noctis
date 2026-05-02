/*
 * noctis — minimal tiling Wayland compositor
 *
 * Config: ~/.config/noctis/config.nox
 * Lang:   C++17
 */

#define _POSIX_C_SOURCE 200809L

extern "C" {
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

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
}

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>

/* ── Config ─────────────────────────────────────────────────────────────── */

struct NocKeybind {
    uint32_t     modifiers;
    xkb_keysym_t sym;
    std::string  action; // "exec:cmd", "close", "focus_next", "focus_prev", "exit"
};

struct NocConfig {
    std::string terminal     = "kitty";
    int         gap          = 6;
    float       master_ratio = 0.55f;
    int         border_width = 2;
    std::string wallpaper    = "";

    float bg[4]              = {0.10f, 0.10f, 0.15f, 1.0f};
    float active_border[4]   = {0.67f, 0.42f, 0.42f, 1.0f};
    float inactive_border[4] = {0.20f, 0.20f, 0.20f, 1.0f};

    std::vector<std::string> autostart;
    std::vector<NocKeybind>  keybinds;
};

static NocConfig cfg;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool parse_hex_color(const std::string &hex, float out[4]) {
    if (hex.empty() || hex[0] != '#') return false;
    std::string h = hex.substr(1);
    if (h.size() != 6 && h.size() != 8) return false;
    unsigned r, g, b, a = 255;
    if (h.size() == 6) sscanf(h.c_str(), "%02x%02x%02x", &r, &g, &b);
    else               sscanf(h.c_str(), "%02x%02x%02x%02x", &r, &g, &b, &a);
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
    return true;
}

static std::string expand_home(const std::string &path) {
    if (!path.empty() && path[0] == '~') {
        const char *home = getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
    }
    return path;
}

/*
 * Parse bind line:
 *   bind = SUPER, Return, exec, kitty
 *   bind = SUPER SHIFT, Q, exit
 *   bind = SUPER, J, focus_next
 */
static bool parse_bind(const std::string &val, NocKeybind &out) {
    // split by comma, max 4 parts
    std::vector<std::string> parts;
    std::istringstream ss(val);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        parts.push_back(trim(tok));
    }
    if (parts.size() < 3) {
        fprintf(stderr, "[noctis] ERROR: bind needs at least 3 parts (mods, key, action): %s\n",
                val.c_str());
        return false;
    }

    // modifiers — space separated in parts[0]
    out.modifiers = 0;
    std::istringstream mss(parts[0]);
    while (mss >> tok) {
        std::string m = tok;
        std::transform(m.begin(), m.end(), m.begin(), ::toupper);
        if (m == "SUPER" || m == "LOGO")   out.modifiers |= WLR_MODIFIER_LOGO;
        else if (m == "SHIFT")             out.modifiers |= WLR_MODIFIER_SHIFT;
        else if (m == "CTRL" || m == "CONTROL") out.modifiers |= WLR_MODIFIER_CTRL;
        else if (m == "ALT")               out.modifiers |= WLR_MODIFIER_ALT;
        else {
            fprintf(stderr, "[noctis] ERROR: unknown modifier \"%s\" in bind: %s\n",
                    tok.c_str(), val.c_str());
            return false;
        }
    }

    // key symbol
    out.sym = xkb_keysym_from_name(parts[1].c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (out.sym == XKB_KEY_NoSymbol) {
        fprintf(stderr, "[noctis] ERROR: unknown key \"%s\" in bind: %s\n",
                parts[1].c_str(), val.c_str());
        return false;
    }

    // action
    std::string action = parts[2];
    if (action == "exec") {
        std::string cmd = parts.size() >= 4 ? parts[3] : "";
        // collect remaining parts (cmd might have commas)
        for (size_t i = 4; i < parts.size(); i++) cmd += "," + parts[i];
        out.action = "exec:" + trim(cmd);
    } else {
        out.action = action;
    }

    return true;
}

/* ── .nox parser ─────────────────────────────────────────────────────────── */
/*
 * Format:
 *
 *   # comment
 *   general {
 *       terminal = kitty
 *       gap = 6
 *       wallpaper = ~/.config/noctis/wallpaper.jpg
 *   }
 *   colors {
 *       background = #1a1a26
 *       active_border = #AB6C6A
 *   }
 *   autostart {
 *       exec = waybar
 *   }
 *   bind = SUPER, Return, exec, kitty
 *   bind = SUPER, Q, close
 */

static void setup_default_binds() {
    struct { const char *mods; const char *key; const char *action; } defs[] = {
        { "SUPER",       "Return", "exec:kitty"  },
        { "SUPER",       "Q",      "close"       },
        { "SUPER",       "J",      "focus_next"  },
        { "SUPER",       "K",      "focus_prev"  },
        { "SUPER SHIFT", "Q",      "exit"        },
    };
    for (auto &d : defs) {
        std::string val = std::string(d.mods) + ", " + d.key + ", " + d.action;
        NocKeybind kb;
        if (parse_bind(val, kb))
            cfg.keybinds.push_back(kb);
    }
}

static void config_load() {
    // defaults already set by NocConfig initializers
    setup_default_binds();

    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[512];
    if (xdg && *xdg)
        snprintf(path, sizeof(path), "%s/noctis/config.nox", xdg);
    else if (home)
        snprintf(path, sizeof(path), "%s/.config/noctis/config.nox", home);
    else {
        wlr_log(WLR_WARN, "Cannot find config dir, using defaults");
        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        wlr_log(WLR_INFO, "No config at %s — using defaults", path);
        return;
    }
    wlr_log(WLR_INFO, "Loading config: %s", path);

    char line[1024];
    std::string section;

    while (fgets(line, sizeof(line), f)) {
        std::string l = trim(std::string(line));

        // skip comments and empty lines
        if (l.empty() || l[0] == '#') continue;

        // section open: "general {"
        if (l.back() == '{') {
            section = trim(l.substr(0, l.size() - 1));
            continue;
        }

        // section close
        if (l == "}") {
            section.clear();
            continue;
        }

        // top-level bind
        if (l.substr(0, 4) == "bind" && l.find('=') != std::string::npos) {
            size_t eq = l.find('=');
            std::string val = trim(l.substr(eq + 1));
            NocKeybind kb;
            if (parse_bind(val, kb))
                cfg.keybinds.push_back(kb);
            continue;
        }

        // key = value inside section
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(l.substr(0, eq));
        std::string val = trim(l.substr(eq + 1));

        if (section == "general") {
            if      (key == "terminal")     cfg.terminal     = val;
            else if (key == "gap")          cfg.gap          = atoi(val.c_str());
            else if (key == "master_ratio") cfg.master_ratio = (float)atof(val.c_str());
            else if (key == "border_width") cfg.border_width = atoi(val.c_str());
            else if (key == "wallpaper")    cfg.wallpaper    = expand_home(val);
        }
        else if (section == "colors") {
            float tmp[4];
            if      (key == "background"      && parse_hex_color(val, tmp)) memcpy(cfg.bg,              tmp, sizeof(tmp));
            else if (key == "active_border"   && parse_hex_color(val, tmp)) memcpy(cfg.active_border,   tmp, sizeof(tmp));
            else if (key == "inactive_border" && parse_hex_color(val, tmp)) memcpy(cfg.inactive_border, tmp, sizeof(tmp));
        }
        else if (section == "autostart") {
            if (key == "exec") cfg.autostart.push_back(val);
        }
    }

    fclose(f);
    wlr_log(WLR_INFO, "Config loaded — terminal=%s gap=%d binds=%zu autostart=%zu",
            cfg.terminal.c_str(), cfg.gap,
            cfg.keybinds.size(), cfg.autostart.size());
}

/* ── Compositor structs ──────────────────────────────────────────────────── */

struct NocServer {
    wl_display             *display      = nullptr;
    wlr_backend            *backend      = nullptr;
    wlr_renderer           *renderer     = nullptr;
    wlr_allocator          *allocator    = nullptr;
    wlr_scene              *scene        = nullptr;
    wlr_scene_output_layout *scene_layout = nullptr;

    wlr_xdg_shell          *xdg_shell   = nullptr;
    wl_listener             new_xdg_surface;

    wlr_cursor             *cursor      = nullptr;
    wlr_xcursor_manager    *cursor_mgr  = nullptr;
    wl_listener             cursor_motion;
    wl_listener             cursor_motion_absolute;
    wl_listener             cursor_button;
    wl_listener             cursor_axis;
    wl_listener             cursor_frame;

    wlr_seat               *seat        = nullptr;
    wl_listener             request_cursor;
    wl_listener             request_set_selection;

    wlr_output_layout      *output_layout = nullptr;
    wl_list                 outputs;
    wl_listener             new_output;
    wl_listener             new_input;

    wl_list                 views;
    struct NocView         *focused_view = nullptr;
};

struct NocOutput {
    wl_list             link;
    NocServer          *server;
    wlr_output         *wlr_output;
    wlr_scene_output   *scene_output;
    wl_listener         frame;
    wl_listener         request_state;
    wl_listener         destroy;
};

struct NocView {
    wl_list             link;
    NocServer          *server;
    wlr_xdg_toplevel   *toplevel;
    wlr_scene_tree     *scene_tree;
    bool                mapped = false;
    int x = 0, y = 0, w = 0, h = 0;
    wl_listener         map;
    wl_listener         unmap;
    wl_listener         destroy;
    wl_listener         request_move;
    wl_listener         request_resize;
    wl_listener         request_maximize;
    wl_listener         request_fullscreen;
};

struct NocKeyboard {
    wl_list             link;
    NocServer          *server;
    wlr_keyboard       *wlr_keyboard;
    wl_listener         modifiers;
    wl_listener         key;
    wl_listener         destroy;
};

/* ── Spawn ───────────────────────────────────────────────────────────────── */

static void spawn(const std::string &cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        pid_t inner = fork();
        if (inner == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

/* ── Layout ──────────────────────────────────────────────────────────────── */

static void apply_layout(NocServer *server) {
    wlr_box box = {};
    wlr_output_layout_get_box(server->output_layout, nullptr, &box);
    if (box.width == 0 || box.height == 0) return;

    std::vector<NocView *> views;
    NocView *v;
    wl_list_for_each(v, &server->views, link) {
        if (v->mapped) views.push_back(v);
    }
    if (views.empty()) return;

    int g  = cfg.gap;
    int ax = box.x + g, ay = box.y + g;
    int aw = box.width - 2 * g, ah = box.height - 2 * g;
    int n  = (int)views.size();

    if (n == 1) {
        wlr_scene_node_set_position(&views[0]->scene_tree->node, ax, ay);
        wlr_xdg_toplevel_set_size(views[0]->toplevel,
            (uint32_t)(aw > 0 ? aw : 1), (uint32_t)(ah > 0 ? ah : 1));
        views[0]->x = ax; views[0]->y = ay;
        views[0]->w = aw; views[0]->h = ah;
        return;
    }

    int mw = (int)(aw * cfg.master_ratio) - g / 2;
    int sx = ax + mw + g;
    int sw = aw - mw - g;

    wlr_scene_node_set_position(&views[0]->scene_tree->node, ax, ay);
    wlr_xdg_toplevel_set_size(views[0]->toplevel,
        (uint32_t)(mw > 0 ? mw : 1), (uint32_t)(ah > 0 ? ah : 1));
    views[0]->x = ax; views[0]->y = ay;
    views[0]->w = mw; views[0]->h = ah;

    int ns   = n - 1;
    int each = (ah - (ns - 1) * g) / ns;
    for (int i = 0; i < ns; i++) {
        int sy = ay + i * (each + g);
        wlr_scene_node_set_position(&views[i+1]->scene_tree->node, sx, sy);
        wlr_xdg_toplevel_set_size(views[i+1]->toplevel,
            (uint32_t)(sw > 0 ? sw : 1), (uint32_t)(each > 0 ? each : 1));
        views[i+1]->x = sx; views[i+1]->y = sy;
        views[i+1]->w = sw; views[i+1]->h = each;
    }
}

/* ── Focus ───────────────────────────────────────────────────────────────── */

static void focus_view(NocServer *server, NocView *view) {
    if (!view || !view->mapped) return;
    NocView *prev = server->focused_view;
    if (prev == view) return;
    if (prev) wlr_xdg_toplevel_set_activated(prev->toplevel, false);
    server->focused_view = view;
    wlr_xdg_toplevel_set_activated(view->toplevel, true);
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
    if (kb)
        wlr_seat_keyboard_notify_enter(server->seat,
            view->toplevel->base->surface,
            kb->keycodes, kb->num_keycodes, &kb->modifiers);
}

static void focus_next(NocServer *server) {
    std::vector<NocView *> views;
    NocView *v;
    wl_list_for_each(v, &server->views, link) { if (v->mapped) views.push_back(v); }
    if (views.empty()) return;
    int idx = 0;
    for (int i = 0; i < (int)views.size(); i++)
        if (views[i] == server->focused_view) { idx = (i + 1) % (int)views.size(); break; }
    focus_view(server, views[idx]);
}

static void focus_prev(NocServer *server) {
    std::vector<NocView *> views;
    NocView *v;
    wl_list_for_each(v, &server->views, link) { if (v->mapped) views.push_back(v); }
    if (views.empty()) return;
    int idx = 0;
    for (int i = 0; i < (int)views.size(); i++)
        if (views[i] == server->focused_view) { idx = (i == 0) ? (int)views.size() - 1 : i - 1; break; }
    focus_view(server, views[idx]);
}

/* ── Surface hit-test ────────────────────────────────────────────────────── */

static wlr_surface *desktop_surface_at(NocServer *server,
                                        double lx, double ly,
                                        double *sx, double *sy) {
    wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return nullptr;
    wlr_scene_buffer  *sbuf  = wlr_scene_buffer_from_node(node);
    wlr_scene_surface *ssurf = wlr_scene_surface_try_from_buffer(sbuf);
    if (!ssurf) return nullptr;
    return ssurf->surface;
}

/* ── Keybind dispatch ────────────────────────────────────────────────────── */

static bool handle_keybind(NocServer *server, uint32_t mods, xkb_keysym_t sym) {
    for (auto &kb : cfg.keybinds) {
        if (kb.modifiers != mods || kb.sym != sym) continue;
        if (kb.action.substr(0, 5) == "exec:") {
            spawn(kb.action.substr(5));
        } else if (kb.action == "close") {
            if (server->focused_view)
                wlr_xdg_toplevel_send_close(server->focused_view->toplevel);
        } else if (kb.action == "focus_next") {
            focus_next(server);
        } else if (kb.action == "focus_prev") {
            focus_prev(server);
        } else if (kb.action == "exit") {
            wlr_log(WLR_INFO, "Exiting noctis");
            wl_display_terminate(server->display);
        }
        return true;
    }
    return false;
}

/* ── Keyboard ────────────────────────────────────────────────────────────── */

static void keyboard_handle_modifiers(wl_listener *listener, void *) {
    NocKeyboard *kb = wl_container_of(listener, kb, modifiers);
    wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(wl_listener *listener, void *data) {
    NocKeyboard *kb = wl_container_of(listener, kb, key);
    NocServer   *server = kb->server;
    wlr_keyboard_key_event *ev = static_cast<wlr_keyboard_key_event *>(data);

    uint32_t keycode = ev->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state, keycode, &syms);
    uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

    bool handled = false;
    if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED)
        for (int i = 0; i < nsyms && !handled; i++)
            handled = handle_keybind(server, mods, syms[i]);

    if (!handled) {
        wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat, ev->time_msec, ev->keycode, ev->state);
    }
}

static void keyboard_handle_destroy(wl_listener *listener, void *) {
    NocKeyboard *kb = wl_container_of(listener, kb, destroy);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->destroy.link);
    wl_list_remove(&kb->link);
    delete kb;
}

static void new_keyboard(NocServer *server, wlr_input_device *device) {
    wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(device);

    NocKeyboard *kb  = new NocKeyboard{};
    kb->server       = server;
    kb->wlr_keyboard = wlr_kb;

    xkb_context *ctx    = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap  *keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
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

static void new_pointer(NocServer *server, wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
    wlr_log(WLR_INFO, "New pointer: %s", device->name);
}

/* ── Input handler ───────────────────────────────────────────────────────── */

static void server_new_input(wl_listener *listener, void *data) {
    NocServer *server = wl_container_of(listener, server, new_input);
    wlr_input_device *device = static_cast<wlr_input_device *>(data);
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: new_keyboard(server, device); break;
    case WLR_INPUT_DEVICE_POINTER:  new_pointer(server, device);  break;
    default: break;
    }
    wlr_seat_set_capabilities(server->seat,
        WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

/* ── Cursor ──────────────────────────────────────────────────────────────── */

static void process_cursor_motion(NocServer *server, uint32_t time_msec) {
    double sx, sy;
    wlr_surface *surface = desktop_surface_at(server,
        server->cursor->x, server->cursor->y, &sx, &sy);
    if (!surface) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
        wlr_seat_pointer_clear_focus(server->seat);
    } else {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
    }
}

static void cursor_motion(wl_listener *listener, void *data) {
    NocServer *server = wl_container_of(listener, server, cursor_motion);
    wlr_pointer_motion_event *ev = static_cast<wlr_pointer_motion_event *>(data);
    wlr_cursor_move(server->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    process_cursor_motion(server, ev->time_msec);
}

static void cursor_motion_absolute(wl_listener *listener, void *data) {
    NocServer *server = wl_container_of(listener, server, cursor_motion_absolute);
    wlr_pointer_motion_absolute_event *ev = static_cast<wlr_pointer_motion_absolute_event *>(data);
    wlr_cursor_warp_absolute(server->cursor, &ev->pointer->base, ev->x, ev->y);
    process_cursor_motion(server, ev->time_msec);
}

static void cursor_button(wl_listener *listener, void *data) {
    NocServer *server = wl_container_of(listener, server, cursor_button);
    wlr_pointer_button_event *ev = static_cast<wlr_pointer_button_event *>(data);
    wlr_seat_pointer_notify_button(server->seat, ev->time_msec, ev->button, ev->state);

    if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        double sx, sy;
        wlr_surface *surface = desktop_surface_at(server,
            server->cursor->x, server->cursor->y, &sx, &sy);
        if (surface) {
            wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
            if (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
                NocView *v;
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

static void cursor_axis(wl_listener *listener, void *data) {
    NocServer *server = wl_container_of(listener, server, cursor_axis);
    wlr_pointer_axis_event *ev = static_cast<wlr_pointer_axis_event *>(data);
    wlr_seat_pointer_notify_axis(server->seat, ev->time_msec,
        ev->orientation, ev->delta, ev->delta_discrete, ev->source
#if defined(WLR_018)
        , ev->relative_direction
#endif
    );
}

static void cursor_frame(wl_listener *listener, void *) {
    NocServer *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

/* ── Seat ────────────────────────────────────────────────────────────────── */

static void seat_request_cursor(wl_listener *listener, void *data) {
    NocServer *server = wl_container_of(listener, server, request_cursor);
    wlr_seat_pointer_request_set_cursor_event *ev =
        static_cast<wlr_seat_pointer_request_set_cursor_event *>(data);
    if (ev->seat_client == server->seat->pointer_state.focused_client)
        wlr_cursor_set_surface(server->cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void seat_request_set_selection(wl_listener *listener, void *data) {
    NocServer *server = wl_container_of(listener, server, request_set_selection);
    wlr_seat_request_set_selection_event *ev =
        static_cast<wlr_seat_request_set_selection_event *>(data);
    wlr_seat_set_selection(server->seat, ev->source, ev->serial);
}

/* ── Views ───────────────────────────────────────────────────────────────── */

static void view_map(wl_listener *listener, void *) {
    NocView *view = wl_container_of(listener, view, map);
    view->mapped = true;
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
    focus_view(view->server, view);
    apply_layout(view->server);
}

static void view_unmap(wl_listener *listener, void *) {
    NocView *view = wl_container_of(listener, view, unmap);
    view->mapped = false;
    if (view->server->focused_view == view) {
        view->server->focused_view = nullptr;
        NocView *v;
        wl_list_for_each(v, &view->server->views, link) {
            if (v != view && v->mapped) { focus_view(view->server, v); break; }
        }
    }
    apply_layout(view->server);
}

static void view_destroy(wl_listener *listener, void *) {
    NocView *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->link);
    delete view;
}

static void view_request_move    (wl_listener *, void *) {}
static void view_request_resize  (wl_listener *, void *) {}

static void view_request_maximize(wl_listener *listener, void *) {
    NocView *v = wl_container_of(listener, v, request_maximize);
    wlr_xdg_toplevel_set_maximized(v->toplevel, false);
    wlr_xdg_surface_schedule_configure(v->toplevel->base);
}

static void view_request_fullscreen(wl_listener *listener, void *) {
    NocView *v = wl_container_of(listener, v, request_fullscreen);
    wlr_xdg_toplevel_set_fullscreen(v->toplevel, false);
    wlr_xdg_surface_schedule_configure(v->toplevel->base);
}

static void server_new_xdg_surface(wl_listener *listener, void *data) {
    NocServer *server = wl_container_of(listener, server, new_xdg_surface);
    wlr_xdg_surface *xdg_surface = static_cast<wlr_xdg_surface *>(data);
    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;

    NocView *view      = new NocView{};
    view->server       = server;
    view->toplevel     = xdg_surface->toplevel;
    view->scene_tree   = wlr_scene_xdg_surface_create(&server->scene->tree, xdg_surface);
    view->mapped       = false;

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
    wl_signal_add(&xdg_surface->toplevel->events.request_move, &view->request_move);
    view->request_resize.notify = view_request_resize;
    wl_signal_add(&xdg_surface->toplevel->events.request_resize, &view->request_resize);
    view->request_maximize.notify = view_request_maximize;
    wl_signal_add(&xdg_surface->toplevel->events.request_maximize, &view->request_maximize);
    view->request_fullscreen.notify = view_request_fullscreen;
    wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen, &view->request_fullscreen);

    wl_list_insert(&server->views, &view->link);
    wlr_log(WLR_INFO, "New toplevel: %s",
            xdg_surface->toplevel->app_id ? xdg_surface->toplevel->app_id : "(unknown)");
}

/* ── Output ──────────────────────────────────────────────────────────────── */

static void output_frame(wl_listener *listener, void *) {
    NocOutput *output = wl_container_of(listener, output, frame);
    wlr_scene_output_commit(output->scene_output, nullptr);
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void output_request_state(wl_listener *listener, void *data) {
    NocOutput *output = wl_container_of(listener, output, request_state);
    wlr_output_commit_state(output->wlr_output,
        static_cast<const wlr_output_state *>(data));
}

static void output_destroy(wl_listener *listener, void *) {
    NocOutput *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    delete output;
}

static void server_new_output(wl_listener *listener, void *data) {
    NocServer *server = wl_container_of(listener, server, new_output);
    wlr_output *wlr_output = static_cast<struct wlr_output *>(data);

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) wlr_output_state_set_mode(&state, mode);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    NocOutput *output    = new NocOutput{};
    output->server       = server;
    output->wlr_output   = wlr_output;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wlr_output_layout_output *l_out =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_out, output->scene_output);

    wl_list_insert(&server->outputs, &output->link);
    wlr_log(WLR_INFO, "New output: %s (%dx%d)",
            wlr_output->name, wlr_output->width, wlr_output->height);
    apply_layout(server);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    signal(SIGCHLD, SIG_IGN);

    wlr_log_init(WLR_DEBUG, nullptr);
    config_load();

    NocServer server = {};
    wl_list_init(&server.outputs);
    wl_list_init(&server.views);

    server.display = wl_display_create();

#if defined(WLR_018)
    wl_event_loop *loop = wl_display_get_event_loop(server.display);
    server.backend = wlr_backend_autocreate(loop, nullptr);
#else
    server.backend = wlr_backend_autocreate(server.display, nullptr);
#endif
    if (!server.backend) {
        wlr_log(WLR_ERROR, "Failed to create backend");
        return 1;
    }

    server.renderer  = wlr_renderer_autocreate(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.display);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

    wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

#if defined(WLR_018)
    server.output_layout = wlr_output_layout_create(server.display);
#else
    server.output_layout = wlr_output_layout_create();
#endif

    server.scene        = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    /* Solid bg rect — swww will cover it when wallpaper is set */
    {
        wlr_scene_rect *bg = wlr_scene_rect_create(
            &server.scene->tree, 32767, 32767, cfg.bg);
        wlr_scene_node_lower_to_bottom(&bg->node);
    }

    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_surface.notify = server_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

    server.cursor     = wlr_cursor_create();
    server.cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    wlr_xcursor_manager_load(server.cursor_mgr, 1);

    server.cursor_motion.notify          = cursor_motion;
    server.cursor_motion_absolute.notify = cursor_motion_absolute;
    server.cursor_button.notify          = cursor_button;
    server.cursor_axis.notify            = cursor_axis;
    server.cursor_frame.notify           = cursor_frame;
    wl_signal_add(&server.cursor->events.motion,          &server.cursor_motion);
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    wl_signal_add(&server.cursor->events.button,          &server.cursor_button);
    wl_signal_add(&server.cursor->events.axis,            &server.cursor_axis);
    wl_signal_add(&server.cursor->events.frame,           &server.cursor_frame);

    server.seat = wlr_seat_create(server.display, "seat0");
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) { wlr_log(WLR_ERROR, "Failed to create socket"); return 1; }

    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend");
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    wlr_log(WLR_INFO, "noctis running on WAYLAND_DISPLAY=%s", socket);
    wlr_log(WLR_INFO, "Keybinds loaded: %zu", cfg.keybinds.size());

    /* Autostart */
    for (auto &app : cfg.autostart) {
        wlr_log(WLR_INFO, "Autostart: %s", app.c_str());
        spawn(app);
    }

    /* Wallpaper via swww */
    if (!cfg.wallpaper.empty()) {
        wlr_log(WLR_INFO, "Wallpaper: %s", cfg.wallpaper.c_str());
        spawn("swww-daemon");
        // give daemon a moment then set the image
        std::string cmd = "sleep 0.5 && swww img \"" + cfg.wallpaper + "\" --transition-type none";
        spawn(cmd);
    }

    wl_display_run(server.display);

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
