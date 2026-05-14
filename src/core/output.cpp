#include "core/output.hpp"
#include "core/server.hpp"

extern "C" {
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
}

static void cb_output_frame(struct wl_listener *l, void *) {
    NoxOutput *out = wl_container_of(l, out, m_frame);

    struct wlr_scene_output *scene_out =
        wlr_scene_get_scene_output(out->server->scene, out->wlr_output);

    wlr_scene_output_commit(scene_out, nullptr);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_out, &now);
}

static void cb_output_request_state(struct wl_listener *l, void *data) {
    NoxOutput *out = wl_container_of(l, out, m_request_state);
    auto *event = static_cast<struct wlr_output_event_request_state *>(data);
    wlr_output_commit_state(out->wlr_output, event->state);
}

static void cb_output_destroy(struct wl_listener *l, void *) {
    NoxOutput *out = wl_container_of(l, out, m_destroy);
    // Remove from server outputs list
    auto &outs = out->server->outputs;
    outs.erase(std::remove(outs.begin(), outs.end(), out), outs.end());
    delete out;
}

NoxOutput::NoxOutput(NoxServer *srv, struct wlr_output *output)
    : server(srv), wlr_output(output)
{
    m_frame.notify = cb_output_frame;
    wl_signal_add(&output->events.frame, &m_frame);

    m_request_state.notify = cb_output_request_state;
    wl_signal_add(&output->events.request_state, &m_request_state);

    m_destroy.notify = cb_output_destroy;
    wl_signal_add(&output->events.destroy, &m_destroy);
}

NoxOutput::~NoxOutput() {
    wl_list_remove(&m_frame.link);
    wl_list_remove(&m_request_state.link);
    wl_list_remove(&m_destroy.link);
}

int NoxOutput::width() const {
    int w = 0, h = 0;
    wlr_output_effective_resolution(wlr_output, &w, &h);
    return w;
}

int NoxOutput::height() const {
    int w = 0, h = 0;
    wlr_output_effective_resolution(wlr_output, &w, &h);
    return h;
}
