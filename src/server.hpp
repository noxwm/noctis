#pragma once

extern "C" {
#include <wayland-server-core.h>
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
}

#include <vector>
#include <string>

#include "view.hpp"
#include "layout.hpp"
#include "keybinds.hpp"
#include "config.hpp"

struct Output;

struct Server {
    struct wl_display          *display        = nullptr;
    struct wl_event_loop       *event_loop     = nullptr;  // 0.18: backend takes event_loop
    struct wlr_backend         *backend        = nullptr;
    struct wlr_renderer        *renderer       = nullptr;
    struct wlr_allocator       *allocator      = nullptr;
    struct wlr_compositor      *compositor     = nullptr;
    struct wlr_output_layout   *output_layout  = nullptr;
    struct wlr_scene           *scene          = nullptr;
    struct wlr_scene_output_layout *scene_layout = nullptr;

    struct wlr_xdg_shell       *xdg_shell      = nullptr;
    struct wlr_seat            *seat           = nullptr;
    struct wlr_cursor          *cursor         = nullptr;
    struct wlr_xcursor_manager *cursor_mgr     = nullptr;

    std::vector<View *>   views;
    std::vector<Output *> outputs;
    View                 *focused_view = nullptr;

    Layout         layout;
    KeybindManager keybinds;

    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener new_xdg_toplevel;

    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;

    bool init();
    void run();
    void destroy();

    void focus_view(View *view);
    void focus_next();
    void focus_prev();
    void apply_layout();
    Box  get_output_box();
    void spawn(const std::string &cmd);
};
