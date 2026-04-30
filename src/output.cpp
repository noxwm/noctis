#include "output.hpp"
#include "server.hpp"
#include "util.hpp"
#include <algorithm>

extern "C" {
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
}

// ── frame callback ────────────────────────────────────────────────────────────

static void output_handle_frame(struct wl_listener *listener, void * /*data*/) {
    Output *output = CONTAINER_OF(listener, Output, frame);

    struct wlr_scene_output *scene_output = output->scene_output;

    if (!wlr_scene_output_commit(scene_output, nullptr)) {
        LOG_WARN("wlr_scene_output_commit failed");
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

// ── request_state (mode changes etc.) ────────────────────────────────────────

static void output_handle_request_state(struct wl_listener *listener, void *data) {
    Output *output = CONTAINER_OF(listener, Output, request_state);
    const struct wlr_output_state *state =
        reinterpret_cast<const struct wlr_output_state *>(data);
    wlr_output_commit_state(output->wlr_output, state);
}

// ── destroy ───────────────────────────────────────────────────────────────────

static void output_handle_destroy(struct wl_listener *listener, void * /*data*/) {
    Output *output = CONTAINER_OF(listener, Output, destroy);

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);

    Server *server = output->server;
    auto &outs = server->outputs;
    auto it = std::find(outs.begin(), outs.end(), output);
    if (it != outs.end()) outs.erase(it);

    delete output;
}

// ── Output::init ─────────────────────────────────────────────────────────────

void Output::init(Server *srv, struct wlr_output *out) {
    server     = srv;
    wlr_output = out;

    // Bind renderer/allocator to this output
    wlr_output_init_render(out, srv->allocator, srv->renderer);

    // Enable preferred mode
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(out);
    if (mode) wlr_output_state_set_mode(&state, mode);

    wlr_output_commit_state(out, &state);
    wlr_output_state_finish(&state);

    // Add to layout (auto-arrange)
    struct wlr_output_layout_output *l_out =
        wlr_output_layout_add_auto(srv->output_layout, out);

    // Create scene output
    scene_output = wlr_scene_output_create(srv->scene, out);
    wlr_scene_output_layout_add_output(srv->scene_layout, l_out, scene_output);

    // Wire up listeners
    frame.notify = output_handle_frame;
    wl_signal_add(&out->events.frame, &frame);

    request_state.notify = output_handle_request_state;
    wl_signal_add(&out->events.request_state, &request_state);

    destroy.notify = output_handle_destroy;
    wl_signal_add(&out->events.destroy, &destroy);

    LOG_INFO("New output: %s (%dx%d)", out->name, out->width, out->height);
}
