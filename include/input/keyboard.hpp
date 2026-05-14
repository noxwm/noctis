#pragma once

extern "C" {
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_input_device.h>
#include <wayland-server-core.h>
}

class NoxServer;

class NoxKeyboard {
public:
    NoxKeyboard(NoxServer *server, struct wlr_keyboard *keyboard);
    ~NoxKeyboard();

    NoxServer            *server;
    struct wlr_keyboard  *keyboard;

private:
    struct wl_listener m_modifiers;
    struct wl_listener m_key;
    struct wl_listener m_destroy;

    // Returns true if the key was a compositor keybind (consumed)
    bool handle_keybind(uint32_t keycode, uint32_t modifiers);
};
