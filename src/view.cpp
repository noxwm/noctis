#include "view.hpp"
#include "server.hpp"
#include "util.hpp"

extern "C" {
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_compositor.h>
}

// ── helpers ────────────────────────────────────────────────────────────────

static View *view_from_toplevel_listener(struct wl_listener *l, size_t offset) {
    return reinterpret_cast<View *>(reinterpret_cast<char *>(l) - offset);
}

// ── map ────────────────────────────────────────────────────────────────────

static void view_handle_map(struct wl_listener *listener, void * /*data*/) {
    View *view = CONTAINER_OF(listener, View, map);
    view->mapped = true;
    view->raise();
    view->server->focus_view(view);
    view->server->apply_layout();
}

// ── unmap ──────────────────────────────────────────────────────────────────

static void view_handle_unmap(struct wl_listener *listener, void * /*data*/) {
    View *view = CONTAINER_OF(listener, View, unmap);
    view->mapped = false;

    if (view->server->focused_view == view) {
        view->server->focused_view = nullptr;
        // Focus the next available view
        for (auto *v : view->server->views) {
            if (v != view && v->mapped) {
                view->server->focus_view(v);
                break;
            }
        }
    }
    view->server->apply_layout();
}

// ── destroy ────────────────────────────────────────────────────────────────

static void view_handle_destroy(struct wl_listener *listener, void * /*data*/) {
    View *view = CONTAINER_OF(listener, View, destroy);

    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);

    auto &views = view->server->views;
    views.erase(std::remove(views.begin(), views.end(), view), views.end());

    delete view;
}

// ── request_move / resize / maximize / fullscreen ──────────────────────────
// We're a tiling WM — acknowledge but don't honour interactive move/resize.

static void view_handle_request_move(struct wl_listener * /*l*/, void * /*d*/) {}

static void view_handle_request_resize(struct wl_listener * /*l*/, void *data) {
    // Must send a configure; just acknowledge with current size
    // The view's geometry stays under tiling control
}

static void view_handle_request_maximize(struct wl_listener *listener, void * /*data*/) {
    View *view = CONTAINER_OF(listener, View, request_maximize);
    // Acknowledge: deny maximise (tiling handles geometry)
    wlr_xdg_toplevel_set_maximized(view->toplevel, false);
    wlr_xdg_surface_schedule_configure(view->toplevel->base);
}

static void view_handle_request_fullscreen(struct wl_listener *listener, void * /*data*/) {
    View *view = CONTAINER_OF(listener, View, request_fullscreen);
    wlr_xdg_toplevel_set_fullscreen(view->toplevel, false);
    wlr_xdg_surface_schedule_configure(view->toplevel->base);
}

// ── View::init ─────────────────────────────────────────────────────────────

void View::init(Server *srv, struct wlr_xdg_toplevel *tl, struct wlr_scene_tree *tree) {
    server     = srv;
    toplevel   = tl;
    scene_tree = tree;
    mapped     = false;
    x = y = w = h = 0;

    map.notify = view_handle_map;
    wl_signal_add(&tl->base->surface->events.map, &map);

    unmap.notify = view_handle_unmap;
    wl_signal_add(&tl->base->surface->events.unmap, &unmap);

    destroy.notify = view_handle_destroy;
    wl_signal_add(&tl->base->events.destroy, &destroy);

    request_move.notify = view_handle_request_move;
    wl_signal_add(&tl->events.request_move, &request_move);

    request_resize.notify = view_handle_request_resize;
    wl_signal_add(&tl->events.request_resize, &request_resize);

    request_maximize.notify = view_handle_request_maximize;
    wl_signal_add(&tl->events.request_maximize, &request_maximize);

    request_fullscreen.notify = view_handle_request_fullscreen;
    wl_signal_add(&tl->events.request_fullscreen, &request_fullscreen);
}

// ── View::focus ────────────────────────────────────────────────────────────

void View::focus() {
    if (!mapped) return;

    Server *srv = server;
    View *prev = srv->focused_view;

    if (prev == this) return;

    if (prev) {
        // Deactivate old focused view
        wlr_xdg_toplevel_set_activated(prev->toplevel, false);
    }

    srv->focused_view = this;
    wlr_xdg_toplevel_set_activated(toplevel, true);

    struct wlr_keyboard *kb = wlr_seat_get_keyboard(srv->seat);
    if (kb) {
        wlr_seat_keyboard_notify_enter(srv->seat,
            toplevel->base->surface,
            kb->keycodes, kb->num_keycodes,
            &kb->modifiers);
    }
}

// ── View::set_geometry ─────────────────────────────────────────────────────

void View::set_geometry(int nx, int ny, int nw, int nh) {
    x = nx; y = ny; w = nw; h = nh;

    wlr_scene_node_set_position(&scene_tree->node, x, y);
    wlr_xdg_toplevel_set_size(toplevel,
        static_cast<uint32_t>(w > 0 ? w : 1),
        static_cast<uint32_t>(h > 0 ? h : 1));
}

// ── View::raise ────────────────────────────────────────────────────────────

void View::raise() {
    wlr_scene_node_raise_to_top(&scene_tree->node);
}
