#include "server.hpp"
#include "output.hpp"
#include "input.hpp"
#include "view.hpp"
#include "seat.hpp"
#include "util.hpp"
#include "config.hpp"

extern "C" {
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/log.h>
}

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

// ── forward declarations of static handlers ────────────────────────────────

static void server_handle_new_output(struct wl_listener *l, void *data);
static void server_handle_new_input(struct wl_listener *l, void *data);
static void server_handle_new_xdg_toplevel(struct wl_listener *l, void *data);
static void server_handle_request_cursor(struct wl_listener *l, void *data);
static void server_handle_request_set_selection(struct wl_listener *l, void *data);

// Cursor handlers are defined in input.cpp — we re-declare them here for
// wiring; they reference Server members directly via CONTAINER_OF.
extern void handle_new_keyboard(Server *server, struct wlr_input_device *device);
extern void handle_new_pointer(Server *server, struct wlr_input_device *device);

// These are defined in input.cpp and need external linkage:
extern "C" {
    // We cannot make these truly extern — they are static in input.cpp.
    // Instead we expose them via the Server listener fields that are
    // connected in Server::init() below.
}

// ── new_output ─────────────────────────────────────────────────────────────

static void server_handle_new_output(struct wl_listener *listener, void *data) {
    Server *server = CONTAINER_OF(listener, Server, new_output);
    struct wlr_output *wlr_out = reinterpret_cast<struct wlr_output *>(data);

    Output *output = new Output();
    output->init(server, wlr_out);
    server->outputs.push_back(output);

    // Retile with new geometry
    server->apply_layout();
}

// ── new_input ──────────────────────────────────────────────────────────────

static void server_handle_new_input(struct wl_listener *listener, void *data) {
    Server *server = CONTAINER_OF(listener, Server, new_input);
    struct wlr_input_device *device =
        reinterpret_cast<struct wlr_input_device *>(data);

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        handle_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        handle_new_pointer(server, device);
        break;
    default:
        break;
    }

    // Update seat capabilities
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    // Always advertise keyboard capability (even with no physical keyboard)
    caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

// ── new xdg toplevel ───────────────────────────────────────────────────────

static void server_handle_new_xdg_toplevel(struct wl_listener *listener,
                                            void *data) {
    Server *server = CONTAINER_OF(listener, Server, new_xdg_toplevel);
    struct wlr_xdg_surface *xdg_surface =
        reinterpret_cast<struct wlr_xdg_surface *>(data);

    // Only handle toplevels, not popups
    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;

    struct wlr_xdg_toplevel *toplevel = xdg_surface->toplevel;

    // Create a scene tree node for this view
    struct wlr_scene_tree *scene_tree =
        wlr_scene_xdg_surface_create(&server->scene->tree, toplevel->base);

    View *view = new View();
    view->init(server, toplevel, scene_tree);
    server->views.push_back(view);

    LOG_INFO("New toplevel: %s", toplevel->app_id ? toplevel->app_id : "(unknown)");
}

// ── seat: request_cursor ───────────────────────────────────────────────────

static void server_handle_request_cursor(struct wl_listener *listener, void *data) {
    Server *server = CONTAINER_OF(listener, Server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event =
        reinterpret_cast<struct wlr_seat_pointer_request_set_cursor_event *>(data);

    struct wlr_seat_client *focused =
        server->seat->pointer_state.focused_client;

    if (focused == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface,
                               event->hotspot_x, event->hotspot_y);
    }
}

// ── seat: request_set_selection ───────────────────────────────────────────

static void server_handle_request_set_selection(struct wl_listener *listener,
                                                  void *data) {
    Server *server = CONTAINER_OF(listener, Server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event =
        reinterpret_cast<struct wlr_seat_request_set_selection_event *>(data);
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

// ── cursor motion handlers (defined in input.cpp, wired here) ─────────────

// Declared in input.cpp with external linkage via the listener approach.
// We wire them using function pointers stored in server listener fields.

static void _cursor_motion(struct wl_listener *l, void *d) {
    Server *s = CONTAINER_OF(l, Server, cursor_motion);
    struct wlr_pointer_motion_event *e =
        reinterpret_cast<struct wlr_pointer_motion_event *>(d);
    wlr_cursor_move(s->cursor, &e->pointer->base, e->delta_x, e->delta_y);

    // process motion
    double sx, sy;
    struct wlr_surface *surface =
        desktop_surface_at(s, s->cursor->x, s->cursor->y, &sx, &sy);
    if (surface) {
        wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(s->seat, e->time_msec, sx, sy);
    } else {
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "left_ptr");
        wlr_seat_pointer_clear_focus(s->seat);
    }
}

static void _cursor_motion_abs(struct wl_listener *l, void *d) {
    Server *s = CONTAINER_OF(l, Server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *e =
        reinterpret_cast<struct wlr_pointer_motion_absolute_event *>(d);
    wlr_cursor_warp_absolute(s->cursor, &e->pointer->base, e->x, e->y);

    double sx, sy;
    struct wlr_surface *surface =
        desktop_surface_at(s, s->cursor->x, s->cursor->y, &sx, &sy);
    if (surface) {
        wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(s->seat, e->time_msec, sx, sy);
    } else {
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "left_ptr");
        wlr_seat_pointer_clear_focus(s->seat);
    }
}

static void _cursor_button(struct wl_listener *l, void *d) {
    Server *s = CONTAINER_OF(l, Server, cursor_button);
    struct wlr_pointer_button_event *e =
        reinterpret_cast<struct wlr_pointer_button_event *>(d);
    wlr_seat_pointer_notify_button(s->seat, e->time_msec, e->button, e->state);

    if (e->state == WLR_BUTTON_PRESSED) {
        double sx, sy;
        struct wlr_surface *surface =
            desktop_surface_at(s, s->cursor->x, s->cursor->y, &sx, &sy);
        if (surface) {
            struct wlr_xdg_surface *xdg_surf =
                wlr_xdg_surface_try_from_wlr_surface(surface);
            if (xdg_surf &&
                xdg_surf->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
                for (auto *v : s->views) {
                    if (v->toplevel == xdg_surf->toplevel && v->mapped) {
                        s->focus_view(v);
                        break;
                    }
                }
            }
        }
    }
}

static void _cursor_axis(struct wl_listener *l, void *d) {
    Server *s = CONTAINER_OF(l, Server, cursor_axis);
    struct wlr_pointer_axis_event *e =
        reinterpret_cast<struct wlr_pointer_axis_event *>(d);
    wlr_seat_pointer_notify_axis(s->seat, e->time_msec, e->orientation,
        e->delta, e->delta_discrete, e->source);
}

static void _cursor_frame(struct wl_listener *l, void * /*d*/) {
    Server *s = CONTAINER_OF(l, Server, cursor_frame);
    wlr_seat_pointer_notify_frame(s->seat);
}

// ── Server::init ───────────────────────────────────────────────────────────

bool Server::init() {
    wlr_log_init(WLR_DEBUG, nullptr);
    g_config.load();

    display = wl_display_create();
    if (!display) {
        LOG_ERROR("Failed to create Wayland display");
        return false;
    }

    // Backend (autodetects DRM/libinput, Wayland nested, X11 nested)
    backend = wlr_backend_autocreate(display, nullptr);
    if (!backend) {
        LOG_ERROR("Failed to create wlroots backend");
        return false;
    }

    renderer = wlr_renderer_autocreate(backend);
    if (!renderer) {
        LOG_ERROR("Failed to create renderer");
        return false;
    }
    wlr_renderer_init_wl_display(renderer, display);

    allocator = wlr_allocator_autocreate(backend, renderer);
    if (!allocator) {
        LOG_ERROR("Failed to create allocator");
        return false;
    }

    // Globals
    compositor = wlr_compositor_create(display, 5, renderer);
    wlr_subcompositor_create(display);
    wlr_data_device_manager_create(display);

    // Output layout + scene
    output_layout = wlr_output_layout_create();
    scene         = wlr_scene_create();
    scene_layout  = wlr_scene_attach_output_layout(scene, output_layout);

    // XDG shell
    xdg_shell = wlr_xdg_shell_create(display, 3);
    new_xdg_toplevel.notify = server_handle_new_xdg_toplevel;
    wl_signal_add(&xdg_shell->events.new_surface, &new_xdg_toplevel);

    // Seat
    seat = wlr_seat_create(display, "seat0");
    request_cursor.notify = server_handle_request_cursor;
    wl_signal_add(&seat->events.request_set_cursor, &request_cursor);

    request_set_selection.notify = server_handle_request_set_selection;
    wl_signal_add(&seat->events.request_set_selection, &request_set_selection);

    // Cursor
    cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(cursor, output_layout);

    cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);
    wlr_xcursor_manager_load(cursor_mgr, 1);

    cursor_motion.notify           = _cursor_motion;
    cursor_motion_absolute.notify  = _cursor_motion_abs;
    cursor_button.notify           = _cursor_button;
    cursor_axis.notify             = _cursor_axis;
    cursor_frame.notify            = _cursor_frame;

    wl_signal_add(&cursor->events.motion,          &cursor_motion);
    wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
    wl_signal_add(&cursor->events.button,          &cursor_button);
    wl_signal_add(&cursor->events.axis,            &cursor_axis);
    wl_signal_add(&cursor->events.frame,           &cursor_frame);

    // Backend input/output events
    new_output.notify = server_handle_new_output;
    wl_signal_add(&backend->events.new_output, &new_output);

    new_input.notify = server_handle_new_input;
    wl_signal_add(&backend->events.new_input, &new_input);

    // Keybinds
    keybinds.setup_defaults(this);

    // Expose Wayland socket
    const char *socket = wl_display_add_socket_auto(display);
    if (!socket) {
        LOG_ERROR("Failed to add Wayland socket");
        return false;
    }

    // Start backend
    if (!wlr_backend_start(backend)) {
        LOG_ERROR("Failed to start backend");
        return false;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    LOG_INFO("Compositor running on WAYLAND_DISPLAY=%s", socket);

    // Run autostart entries from config
    for (auto &entry : g_config.autostart) {
        LOG_INFO("Autostart: %s", entry.cmd.c_str());
        spawn(entry.cmd);
    }

    return true;
}

// ── Server::run ────────────────────────────────────────────────────────────

void Server::run() {
    wl_display_run(display);
}

// ── Server::destroy ────────────────────────────────────────────────────────

void Server::destroy() {
    wl_display_destroy_clients(display);
    wlr_scene_node_destroy(&scene->tree.node);
    wlr_xcursor_manager_destroy(cursor_mgr);
    wlr_cursor_destroy(cursor);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);
}

// ── Server::focus_view ─────────────────────────────────────────────────────

void Server::focus_view(View *view) {
    if (!view || !view->mapped) return;
    view->focus();
}

// ── Server::focus_next / focus_prev ───────────────────────────────────────

void Server::focus_next() {
    std::vector<View *> mapped;
    for (auto *v : views) { if (v->mapped) mapped.push_back(v); }
    if (mapped.empty()) return;

    size_t idx = 0;
    for (size_t i = 0; i < mapped.size(); ++i) {
        if (mapped[i] == focused_view) { idx = (i + 1) % mapped.size(); break; }
    }
    focus_view(mapped[idx]);
}

void Server::focus_prev() {
    std::vector<View *> mapped;
    for (auto *v : views) { if (v->mapped) mapped.push_back(v); }
    if (mapped.empty()) return;

    size_t idx = 0;
    for (size_t i = 0; i < mapped.size(); ++i) {
        if (mapped[i] == focused_view) {
            idx = (i == 0) ? mapped.size() - 1 : i - 1;
            break;
        }
    }
    focus_view(mapped[idx]);
}

// ── Server::apply_layout ───────────────────────────────────────────────────

void Server::apply_layout() {
    Box area = get_output_box();
    if (area.w == 0 || area.h == 0) return;
    layout.tile(views, area, g_config.master_ratio, g_config.gap);
}

// ── Server::get_output_box ─────────────────────────────────────────────────

Box Server::get_output_box() {
    struct wlr_box box = {};
    wlr_output_layout_get_box(output_layout, nullptr, &box);
    return {box.x, box.y, box.width, box.height};
}

// ── Server::spawn ──────────────────────────────────────────────────────────

void Server::spawn(const std::string &cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child: double-fork to avoid zombie
        pid_t inner = fork();
        if (inner == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        // Reap immediate child
        int status;
        waitpid(pid, &status, 0);
    }
}
