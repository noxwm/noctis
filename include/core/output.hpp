#pragma once

extern "C" {
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wayland-server-core.h>
}

class NoxServer;

class NoxOutput {
public:
    NoxOutput(NoxServer *server, struct wlr_output *output);
    ~NoxOutput();

    NoxServer          *server;
    struct wlr_output  *wlr_output;
    struct wlr_scene_output *scene_output;

    int width()  const;
    int height() const;

private:
    struct wl_listener m_frame;
    struct wl_listener m_request_state;
    struct wl_listener m_destroy;
};
