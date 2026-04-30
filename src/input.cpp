#include "input.hpp"
#include "server.hpp"
#include "keybinds.hpp"
#include "util.hpp"
#include "seat.hpp"

extern "C" {
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <xkbcommon/xkbcommon.h>
}

// ── Keyboard handlers ─────────────────────────────────────────────────────────

static void kb_handle_modifiers(struct wl_listener *listener, void * /*data*/) {
    Keyboard *kb = CONTAINER_OF(listener, Keyboard, modifiers);
    wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(kb->server->seat,
        &kb->wlr_keyboard->modifiers);
}

static void kb_handle_key(struct wl_listener *listener, void *data) {
    Keyboard *kb = CONTAINER_OF(listener, Keyboard, key);
    Server *server = kb->server;
    struct wlr_keyboard_key_event *event =
        reinterpret_cast<struct wlr_keyboard_key_event *>(data);

    // Translate to keysym
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(kb->wlr_keyboard->xkb_state,
                                        keycode, &syms);

    uint32_t modifiers = wlr_keyboard_get_modifiers(kb->wlr_keyboard);

    bool handled = false;
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms && !handled; ++i) {
            handled = server->keybinds.handle_key(server, modifiers, syms[i]);
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
            event->keycode, event->state);
    }
}

static void kb_handle_destroy(struct wl_listener *listener, void * /*data*/) {
    Keyboard *kb = CONTAINER_OF(listener, Keyboard, destroy);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->destroy.link);
    delete kb;
}

void Keyboard::init(Server *srv, struct wlr_keyboard *kb) {
    server       = srv;
    wlr_keyboard = kb;

    // Setup XKB keymap
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap  *keymap = xkb_keymap_new_from_names(
        ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(kb, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);

    wlr_keyboard_set_repeat_info(kb, 25, 600);

    modifiers.notify = kb_handle_modifiers;
    wl_signal_add(&kb->events.modifiers, &modifiers);

    key.notify = kb_handle_key;
    wl_signal_add(&kb->events.key, &key);

    destroy.notify = kb_handle_destroy;
    wl_signal_add(&kb->base.events.destroy, &destroy);

    wlr_seat_set_keyboard(srv->seat, kb);
}

// ── handle_new_keyboard ───────────────────────────────────────────────────────

void handle_new_keyboard(Server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_kb = wlr_keyboard_from_input_device(device);
    Keyboard *kb = new Keyboard();
    kb->init(server, wlr_kb);
    LOG_INFO("New keyboard: %s", device->name);
}

// ── Cursor (pointer) handlers ─────────────────────────────────────────────────

static void process_cursor_motion(Server *server, uint32_t time_msec) {
    double sx, sy;
    struct wlr_surface *surface =
        desktop_surface_at(server, server->cursor->x, server->cursor->y, &sx, &sy);

    if (!surface) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    Server *server = CONTAINER_OF(listener, Server, cursor_motion);
    struct wlr_pointer_motion_event *event =
        reinterpret_cast<struct wlr_pointer_motion_event *>(data);
    wlr_cursor_move(server->cursor, &event->pointer->base,
                    event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    Server *server = CONTAINER_OF(listener, Server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event =
        reinterpret_cast<struct wlr_pointer_motion_absolute_event *>(data);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
                             event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    Server *server = CONTAINER_OF(listener, Server, cursor_button);
    struct wlr_pointer_button_event *event =
        reinterpret_cast<struct wlr_pointer_button_event *>(data);

    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
        event->button, event->state);

    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        // Focus the view under the cursor
        double sx, sy;
        struct wlr_surface *surface =
            desktop_surface_at(server, server->cursor->x, server->cursor->y,
                               &sx, &sy);
        if (surface) {
            // Find which View owns this surface
            struct wlr_xdg_surface *xdg_surface =
                wlr_xdg_surface_try_from_wlr_surface(surface);
            if (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
                for (auto *v : server->views) {
                    if (v->toplevel == xdg_surface->toplevel && v->mapped) {
                        server->focus_view(v);
                        break;
                    }
                }
            }
        }
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    Server *server = CONTAINER_OF(listener, Server, cursor_axis);
    struct wlr_pointer_axis_event *event =
        reinterpret_cast<struct wlr_pointer_axis_event *>(data);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
        event->orientation, event->delta, event->delta_discrete,
        event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void * /*data*/) {
    Server *server = CONTAINER_OF(listener, Server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

void handle_new_pointer(Server *server, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
    LOG_INFO("New pointer: %s", device->name);
}
