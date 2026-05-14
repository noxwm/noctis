#include "input/keyboard.hpp"
#include "core/server.hpp"
#include "config/config.hpp"

extern "C" {
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <xkbcommon/xkbcommon.h>
}

#include <string>

// Map modifier bitmask → string prefix used in .nox config
// e.g. WLR_MODIFIER_LOGO = "Super"
static std::string modifiers_to_str(uint32_t mods) {
    std::string s;
    if (mods & WLR_MODIFIER_LOGO)  s += "Super+";
    if (mods & WLR_MODIFIER_ALT)   s += "Alt+";
    if (mods & WLR_MODIFIER_CTRL)  s += "Ctrl+";
    if (mods & WLR_MODIFIER_SHIFT) s += "Shift+";
    return s;
}

static void cb_keyboard_modifiers(struct wl_listener *l, void *) {
    NoxKeyboard *kb = wl_container_of(l, kb, m_modifiers);
    wlr_seat_set_keyboard(kb->server->seat, kb->keyboard);
    wlr_seat_keyboard_notify_modifiers(kb->server->seat,
        &kb->keyboard->modifiers);
}

static void cb_keyboard_key(struct wl_listener *l, void *data) {
    NoxKeyboard *kb = wl_container_of(l, kb, m_key);
    auto *event = static_cast<struct wlr_keyboard_key_event *>(data);

    // Only handle key press, not release
    if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        wlr_seat_set_keyboard(kb->server->seat, kb->keyboard);
        wlr_seat_keyboard_notify_key(kb->server->seat,
            event->time_msec, event->keycode, event->state);
        return;
    }

    uint32_t mods = wlr_keyboard_get_modifiers(kb->keyboard);
    if (kb->handle_keybind(event->keycode, mods)) return;

    // Not a compositor keybind — forward to focused client
    wlr_seat_set_keyboard(kb->server->seat, kb->keyboard);
    wlr_seat_keyboard_notify_key(kb->server->seat,
        event->time_msec, event->keycode, event->state);
}

static void cb_keyboard_destroy(struct wl_listener *l, void *) {
    NoxKeyboard *kb = wl_container_of(l, kb, m_destroy);
    auto &kbs = kb->server->keyboards;
    kbs.erase(std::remove(kbs.begin(), kbs.end(), kb), kbs.end());
    delete kb;
}

NoxKeyboard::NoxKeyboard(NoxServer *srv, struct wlr_keyboard *kb)
    : server(srv), keyboard(kb)
{
    // Set default keymap (system locale)
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap  *map = xkb_keymap_new_from_names(
        ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(kb, map);
    xkb_keymap_unref(map);
    xkb_context_unref(ctx);

    wlr_keyboard_set_repeat_info(kb, 25, 600);

    m_modifiers.notify = cb_keyboard_modifiers;
    wl_signal_add(&kb->events.modifiers, &m_modifiers);

    m_key.notify = cb_keyboard_key;
    wl_signal_add(&kb->events.key, &m_key);

    m_destroy.notify = cb_keyboard_destroy;
    wl_signal_add(&kb->events.destroy, &m_destroy);
}

NoxKeyboard::~NoxKeyboard() {
    wl_list_remove(&m_modifiers.link);
    wl_list_remove(&m_key.link);
    wl_list_remove(&m_destroy.link);
}

bool NoxKeyboard::handle_keybind(uint32_t keycode, uint32_t modifiers) {
    // Translate keycode → keysym → name
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(
        keyboard->xkb_state, keycode + 8, &syms);

    for (int i = 0; i < nsyms; i++) {
        char name_buf[64];
        xkb_keysym_get_name(syms[i], name_buf, sizeof(name_buf));
        std::string key_name = name_buf;

        std::string combo = modifiers_to_str(modifiers) + key_name;

        // Check against all configured keybinds
        for (const auto &bind : server->config->keybinds) {
            std::string bind_combo = modifiers_to_str(bind.modifiers) + bind.key;
            if (combo != bind_combo) continue;

            // Dispatch action
            const std::string &action = bind.action;

            if (action == "close") {
                server->close_focused();
            } else if (action == "focus_next" || action == "focus right") {
                server->focus_next();
            } else if (action == "focus_prev" || action == "focus left") {
                server->focus_prev();
            } else if (action == "fullscreen") {
                server->toggle_fullscreen();
            } else if (action.substr(0, 10) == "workspace ") {
                int ws = std::stoi(action.substr(10)) - 1;
                server->switch_workspace(ws);
            } else if (action.substr(0, 5) == "exec ") {
                server->exec(action.substr(5));
            }

            return true; // consumed
        }
    }

    return false;
}
