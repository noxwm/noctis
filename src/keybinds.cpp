#include "keybinds.hpp"
#include "server.hpp"
#include "util.hpp"

extern "C" {
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>
}

// ── KeybindManager ─────────────────────────────────────────────────────────

void KeybindManager::add(uint32_t mods, xkb_keysym_t sym,
                         std::function<void(Server *)> fn) {
    binds_.push_back({mods, sym, std::move(fn)});
}

bool KeybindManager::handle_key(Server *server, uint32_t modifiers,
                                 xkb_keysym_t sym) {
    for (auto &b : binds_) {
        if (b.modifiers == modifiers && b.sym == sym) {
            b.action(server);
            return true;
        }
    }
    return false;
}

// ── default bindings ───────────────────────────────────────────────────────
//
//  MOD = WLR_MODIFIER_LOGO (Super)
//
//  MOD+Return  → launch terminal
//  MOD+Q       → close focused window
//  MOD+J       → focus next
//  MOD+K       → focus prev
//  MOD+Shift+Q → exit compositor

void KeybindManager::setup_defaults(Server *server) {
    const uint32_t MOD  = WLR_MODIFIER_LOGO;
    const uint32_t MODS = WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT;

    add(MOD, XKB_KEY_Return, [](Server *s) {
        s->spawn(g_config.terminal);
    });

    add(MOD, XKB_KEY_q, [](Server *s) {
        if (s->focused_view) {
            wlr_xdg_toplevel_send_close(s->focused_view->toplevel);
        }
    });

    add(MOD, XKB_KEY_j, [](Server *s) {
        s->focus_next();
    });

    add(MOD, XKB_KEY_k, [](Server *s) {
        s->focus_prev();
    });

    add(MODS, XKB_KEY_Q, [](Server *s) {
        LOG_INFO("Exiting compositor");
        wl_display_terminate(s->display);
    });

    (void)server; // reserved for future per-server init
}
