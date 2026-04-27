#pragma once

extern "C" {
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wayland-server-core.h>
}

struct Server;

struct Output {
    Server *server;
    struct wlr_output       *wlr_output;
    struct wlr_scene_output *scene_output;

    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;

    void init(Server *srv, struct wlr_output *output);
};
