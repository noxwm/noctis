#pragma once

extern "C" {
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_input_device.h>
#include <wayland-server-core.h>
}

struct Server;

struct Keyboard {
    Server *server;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;

    void init(Server *srv, struct wlr_keyboard *kb);
};

void handle_new_keyboard(Server *server, struct wlr_input_device *device);
void handle_new_pointer(Server *server, struct wlr_input_device *device);
