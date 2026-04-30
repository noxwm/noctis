#include "keybinds.hpp"
#include "server.hpp"
#include "config.hpp"
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

// ── setup_defaults ─────────────────────────────────────────────────────────
// Builds the runtime keybind table from g_config.keybinds.
// Each KeybindEntry action string is dispatched here.

void KeybindManager::setup_defaults(Server *server) {
    (void)server;

    for (auto &entry : g_config.keybinds) {
        const std::string action = entry.action;
        uint32_t     mods = entry.modifiers;
        xkb_keysym_t sym  = entry.sym;

        if (action.rfind("exec:", 0) == 0) {
            // exec:some command
            std::string cmd = action.substr(5);
            add(mods, sym, [cmd](Server *s) {
                s->spawn(cmd);
            });

        } else if (action == "close") {
            add(mods, sym, [](Server *s) {
                if (s->focused_view)
                    wlr_xdg_toplevel_send_close(s->focused_view->toplevel);
            });

        } else if (action == "focus_next") {
            add(mods, sym, [](Server *s) { s->focus_next(); });

        } else if (action == "focus_prev") {
            add(mods, sym, [](Server *s) { s->focus_prev(); });

        } else if (action == "exit") {
            add(mods, sym, [](Server *s) {
                LOG_INFO("Exiting noctis");
                wl_display_terminate(s->display);
            });

        } else {
            // Unknown action — we already validated at parse time,
            // but guard here too.
            fprintf(stderr,
                "[noctis] ERROR: unknown keybind action \"%s\"\n",
                action.c_str());
            exit(1);
        }
    }

    LOG_INFO("Registered %zu keybinds", binds_.size());
}
