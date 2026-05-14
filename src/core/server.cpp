#include "core/server.hpp"
#include "core/output.hpp"
#include "input/keyboard.hpp"
#include "layout/workspace.hpp"
#include "config/config.hpp"

extern "C" {
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
}

#include <cassert>
#include <cstdlib>
#include <cstring>

// ─── C callback trampolines ────────────────────────────────────────────────

#define SERVER_FROM(listener, field) \
    NoxServer *server = wl_container_of(listener, server, field)

static void cb_new_output(struct wl_listener *l, void *data) {
    SERVER_FROM(l, m_new_output);
    server->on_new_output(static_cast<struct wlr_output *>(data));
}

static void cb_new_xdg_toplevel(struct wl_listener *l, void *data) {
    SERVER_FROM(l, m_new_xdg_toplevel);
    server->on_new_xdg_toplevel(static_cast<struct wlr_xdg_toplevel *>(data));
}

static void cb_new_input(struct wl_listener *l, void *data) {
    SERVER_FROM(l, m_new_input);
    server->on_new_input(static_cast<struct wlr_input_device *>(data));
}

static void cb_cursor_motion(struct wl_listener *l, void *data) {
    SERVER_FROM(l, m_cursor_motion);
    server->on_cursor_motion(static_cast<struct wlr_pointer_motion_event *>(data));
}

static void cb_cursor_motion_abs(struct wl_listener *l, void *data) {
    SERVER_FROM(l, m_cursor_motion_abs);
    // Treat absolute as relative for simplicity in v1
    (void)data;
}

static void cb_cursor_button(struct wl_listener *l, void *data) {
    SERVER_FROM(l, m_cursor_button);
    server->on_cursor_button(static_cast<struct wlr_pointer_button_event *>(data));
}

static void cb_cursor_axis(struct wl_listener *l, void *data) {
    SERVER_FROM(l, m_cursor_axis);
    server->on_cursor_axis(static_cast<struct wlr_pointer_axis_event *>(data));
}

static void cb_cursor_frame(struct wl_listener *l, void *) {
    SERVER_FROM(l, m_cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void cb_request_cursor(struct wl_listener *l, void *data) {
    SERVER_FROM(l, m_request_cursor);
    server->on_request_cursor(
        static_cast<struct wlr_seat_pointer_request_set_cursor_event *>(data));
}

// Per-view callbacks
static void cb_view_map(struct wl_listener *l, void *) {
    NoxView *view = wl_container_of(l, view, map);
    // TODO: focus mapped view
}

static void cb_view_unmap(struct wl_listener *l, void *) {
    NoxView *view = wl_container_of(l, view, unmap);
    (void)view;
}

static void cb_view_destroy(struct wl_listener *l, void *) {
    NoxView *view = wl_container_of(l, view, destroy);
    // Remove from server views list (done in server via NoxServer pointer)
    // For now just clean up listeners
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_fullscreen.link);
    delete view;
}

static void cb_view_request_fullscreen(struct wl_listener *l, void *) {
    NoxView *view = wl_container_of(l, view, request_fullscreen);
    // Toggle fullscreen state
    bool want = view->toplevel->requested.fullscreen;
    wlr_xdg_toplevel_set_fullscreen(view->toplevel, want);
    view->is_fullscreen = want;
}

// ─── NoxServer ────────────────────────────────────────────────────────────

NoxServer::NoxServer() {
    config = std::make_unique<NoxConfig>();
}

NoxServer::~NoxServer() {
    if (display) wl_display_destroy_clients(display);
    // wlroots cleans up backend/renderer/etc on wl_display_destroy
    if (display) wl_display_destroy(display);
}

bool NoxServer::init() {
    // Load config
    config->load(NoxConfig::default_path());

    // Create Wayland display
    display = wl_display_create();
    if (!display) return false;

    // Auto-detect backend (DRM on real hardware, headless/x11 in testing)
    backend = wlr_backend_autocreate(wl_display_get_event_loop(display), nullptr);
    if (!backend) return false;

    renderer = wlr_renderer_autocreate(backend);
    if (!renderer) return false;
    wlr_renderer_init_wl_display(renderer, display);

    allocator = wlr_allocator_autocreate(backend, renderer);
    if (!allocator) return false;

    // Global Wayland protocols
    wlr_compositor_create(display, 5, renderer);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);

    // Output layout
    output_layout = wlr_output_layout_create(display);

    // Scene graph — noxwm uses the wlroots scene API for rendering
    scene = wlr_scene_create();
    scene_layout = wlr_scene_attach_output_layout(scene, output_layout);

    // XDG shell (modern Wayland window protocol)
    xdg_shell = wlr_xdg_shell_create(display, 3);

    // Cursor
    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, output_layout);
    cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);

    // Seat (keyboard + pointer abstraction)
    seat = wlr_seat_create(display, "seat0");

    // Workspaces
    // We don't know screen dimensions yet (outputs not connected),
    // so we pass 0,0 and recalculate when an output is added.
    for (int i = 0; i < MAX_WORKSPACES; i++) {
        workspaces[i] = std::make_unique<Workspace>(
            i + 1, 0, 0, config->gaps);
    }
    workspaces[0]->set_active(true);

    // ── Wire up listeners ──────────────────────────────────────────────

    m_new_output.notify = cb_new_output;
    wl_signal_add(&backend->events.new_output, &m_new_output);

    m_new_xdg_toplevel.notify = cb_new_xdg_toplevel;
    wl_signal_add(&xdg_shell->events.new_toplevel, &m_new_xdg_toplevel);

    m_new_input.notify = cb_new_input;
    wl_signal_add(&backend->events.new_input, &m_new_input);

    m_cursor_motion.notify = cb_cursor_motion;
    wl_signal_add(&cursor->events.motion, &m_cursor_motion);

    m_cursor_motion_abs.notify = cb_cursor_motion_abs;
    wl_signal_add(&cursor->events.motion_absolute, &m_cursor_motion_abs);

    m_cursor_button.notify = cb_cursor_button;
    wl_signal_add(&cursor->events.button, &m_cursor_button);

    m_cursor_axis.notify = cb_cursor_axis;
    wl_signal_add(&cursor->events.axis, &m_cursor_axis);

    m_cursor_frame.notify = cb_cursor_frame;
    wl_signal_add(&cursor->events.frame, &m_cursor_frame);

    m_request_cursor.notify = cb_request_cursor;
    wl_signal_add(&seat->events.request_set_cursor, &m_request_cursor);

    return true;
}

void NoxServer::run() {
    // Add socket for Wayland clients
    const char *socket = wl_display_add_socket_auto(display);
    if (!socket) {
        wlr_log(WLR_ERROR, "unable to open Wayland socket");
        return;
    }

    if (!wlr_backend_start(backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        return;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    wlr_log(WLR_INFO, "noxwm running on WAYLAND_DISPLAY=%s", socket);

    // Launch autostart programs
    for (const auto &cmd : config->autostart) {
        exec(cmd);
    }

    wl_display_run(display);
}

// ─── Output ───────────────────────────────────────────────────────────────

void NoxServer::on_new_output(struct wlr_output *wlr_out) {
    wlr_output_init_render(wlr_out, allocator, renderer);

    // Pick the preferred mode
    if (!wl_list_empty(&wlr_out->modes)) {
        struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_out);
        wlr_output_set_mode(wlr_out, mode);
        wlr_output_enable(wlr_out, true);
        if (!wlr_output_commit(wlr_out)) return;
    }

    auto *out = new NoxOutput(this, wlr_out);
    outputs.push_back(out);

    struct wlr_output_layout_output *layout_out =
        wlr_output_layout_add_auto(output_layout, wlr_out);
    struct wlr_scene_output *scene_out =
        wlr_scene_get_scene_output(scene, wlr_out);
    wlr_scene_output_layout_add_output(scene_layout, layout_out, scene_out);

    // Now we know screen size — reinitialise workspace layouts
    for (int i = 0; i < MAX_WORKSPACES; i++) {
        workspaces[i]->layout = std::make_unique<ColumnLayout>(
            out->width(), out->height(), config->gaps);
    }
}

// ─── XDG Shell ────────────────────────────────────────────────────────────

void NoxServer::on_new_xdg_toplevel(struct wlr_xdg_toplevel *toplevel) {
    auto *view = new NoxView();
    view->toplevel    = toplevel;
    view->workspace_id = m_active_ws;

    // Create a scene tree node for this view
    view->scene_tree = wlr_scene_xdg_surface_create(
        &scene->tree, toplevel->base);
    view->scene_tree->node.data = view;
    toplevel->base->data        = view->scene_tree;

    // Wire view listeners
    view->map.notify = cb_view_map;
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);

    view->unmap.notify = cb_view_unmap;
    wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);

    view->destroy.notify = cb_view_destroy;
    wl_signal_add(&toplevel->events.destroy, &view->destroy);

    view->request_fullscreen.notify = cb_view_request_fullscreen;
    wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);

    views.push_back(view);
    workspaces[m_active_ws]->add_view(view);
    apply_layout();
    focus_view(view);
}

// ─── Input ────────────────────────────────────────────────────────────────

void NoxServer::on_new_input(struct wlr_input_device *device) {
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        auto *kb = new NoxKeyboard(this,
            wlr_keyboard_from_input_device(device));
        keyboards.push_back(kb);
        wlr_seat_set_keyboard(seat, kb->keyboard);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        wlr_cursor_attach_input_device(cursor, device);
    }

    // Advertise capabilities to clients
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!keyboards.empty()) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(seat, caps);
}

// ─── Cursor ───────────────────────────────────────────────────────────────

void NoxServer::on_cursor_motion(struct wlr_pointer_motion_event *event) {
    wlr_cursor_move(cursor, &event->pointer->base,
                    event->delta_x, event->delta_y);
    wlr_xcursor_manager_set_cursor_image(cursor_mgr, "left_ptr", cursor);

    struct wlr_surface *surface = nullptr;
    double sx, sy;
    NoxView *view = view_at(cursor->x, cursor->y, &surface, &sx, &sy);

    if (!view) {
        wlr_seat_pointer_clear_focus(seat);
        return;
    }

    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, event->time_msec, sx, sy);
}

void NoxServer::on_cursor_button(struct wlr_pointer_button_event *event) {
    wlr_seat_pointer_notify_button(seat,
        event->time_msec, event->button, event->state);

    if (event->state == WLR_BUTTON_PRESSED) {
        struct wlr_surface *surface = nullptr;
        double sx, sy;
        NoxView *view = view_at(cursor->x, cursor->y, &surface, &sx, &sy);
        if (view) focus_view(view);
    }
}

void NoxServer::on_cursor_axis(struct wlr_pointer_axis_event *event) {
    wlr_seat_pointer_notify_axis(seat,
        event->time_msec, event->orientation,
        event->delta, event->delta_discrete,
        event->source, event->relative_direction);
}

void NoxServer::on_request_cursor(
    struct wlr_seat_pointer_request_set_cursor_event *event)
{
    if (seat->pointer_state.focused_client == event->seat_client) {
        wlr_cursor_set_surface(cursor,
            event->surface, event->hotspot_x, event->hotspot_y);
    }
}

// ─── Focus ────────────────────────────────────────────────────────────────

void NoxServer::focus_view(NoxView *view) {
    if (!view) return;

    NoxView *prev = m_focused;
    m_focused = view;

    // Deactivate previous
    if (prev && prev != view) {
        wlr_xdg_toplevel_set_activated(prev->toplevel, false);
    }

    wlr_xdg_toplevel_set_activated(view->toplevel, true);

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
    if (kb) {
        wlr_seat_keyboard_notify_enter(seat,
            view->toplevel->base->surface,
            kb->keycodes, kb->num_keycodes,
            &kb->modifiers);
    }

    // Raise to top in scene graph
    wlr_scene_node_raise_to_top(&view->scene_tree->node);
}

void NoxServer::focus_next() {
    workspaces[m_active_ws]->focus_next();
    focus_view(workspaces[m_active_ws]->focused_view());
}

void NoxServer::focus_prev() {
    workspaces[m_active_ws]->focus_prev();
    focus_view(workspaces[m_active_ws]->focused_view());
}

// ─── Actions ──────────────────────────────────────────────────────────────

void NoxServer::close_focused() {
    if (!m_focused) return;
    wlr_xdg_toplevel_send_close(m_focused->toplevel);
}

void NoxServer::toggle_fullscreen() {
    if (!m_focused) return;
    bool next = !m_focused->is_fullscreen;
    wlr_xdg_toplevel_set_fullscreen(m_focused->toplevel, next);
    m_focused->is_fullscreen = next;
    apply_layout();
}

void NoxServer::switch_workspace(int id) {
    if (id < 0 || id >= MAX_WORKSPACES) return;
    if (id == m_active_ws) return;

    workspaces[m_active_ws]->set_active(false);
    m_active_ws = id;
    workspaces[m_active_ws]->set_active(true);
    m_focused = workspaces[m_active_ws]->focused_view();
    if (m_focused) focus_view(m_focused);
}

void NoxServer::exec(const std::string &cmd) {
    if (fork() == 0) {
        // Child: launch the command
        execl("/bin/sh", "/bin/sh", "-c", cmd.c_str(), nullptr);
        _exit(1);
    }
}

// ─── Layout ───────────────────────────────────────────────────────────────

void NoxServer::apply_layout() {
    auto &ws = *workspaces[m_active_ws];

    for (auto *view : ws.views) {
        if (view->is_fullscreen && !outputs.empty()) {
            wlr_xdg_toplevel_set_size(view->toplevel,
                outputs[0]->width(), outputs[0]->height());
            wlr_scene_node_set_position(&view->scene_tree->node, 0, 0);
        } else {
            wlr_xdg_toplevel_set_size(view->toplevel,
                view->width, view->height);
            wlr_scene_node_set_position(&view->scene_tree->node,
                view->x, view->y);
        }
    }
}

// ─── Hit-test ─────────────────────────────────────────────────────────────

NoxView *NoxServer::view_at(double lx, double ly,
    struct wlr_surface **surface, double *sx, double *sy)
{
    struct wlr_scene_node *node =
        wlr_scene_node_at(&scene->tree.node, lx, ly, sx, sy);

    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return nullptr;

    auto *scene_buf = wlr_scene_buffer_from_node(node);
    auto *scene_surface = wlr_scene_surface_try_from_buffer(scene_buf);
    if (!scene_surface) return nullptr;

    *surface = scene_surface->surface;

    struct wlr_scene_tree *tree = node->parent;
    while (tree && !tree->node.data) tree = tree->node.parent;

    return tree ? static_cast<NoxView *>(tree->node.data) : nullptr;
}
