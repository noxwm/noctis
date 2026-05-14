#pragma once

#include <memory>
#include <vector>
#include <string>

extern "C" {
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wayland-server-core.h>
}

// Forward declarations
class NoxOutput;
class NoxView;
class NoxKeyboard;
class Workspace;
class ColumnLayout;
class NoxConfig;

// A toplevel window managed by the compositor
struct NoxView {
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_scene_tree   *scene_tree;

    // Which workspace this view belongs to
    int workspace_id;

    // Position and size (managed by layout engine)
    int x, y, width, height;

    bool is_fullscreen = false;

    // wl_listeners
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_fullscreen;
};

class NoxServer {
public:
    NoxServer();
    ~NoxServer();

    bool init();
    void run();

    // Called by event handlers (must be public for C callback access)
    void on_new_output(struct wlr_output *output);
    void on_new_xdg_toplevel(struct wlr_xdg_toplevel *toplevel);
    void on_new_input(struct wlr_input_device *device);
    void on_cursor_motion(struct wlr_pointer_motion_event *event);
    void on_cursor_button(struct wlr_pointer_button_event *event);
    void on_cursor_axis(struct wlr_pointer_axis_event *event);
    void on_request_cursor(struct wlr_seat_pointer_request_set_cursor_event *event);

    // Focus
    void focus_view(NoxView *view);
    void focus_next();
    void focus_prev();

    // Layout
    void close_focused();
    void toggle_fullscreen();
    void switch_workspace(int id);

    // Exec
    void exec(const std::string &cmd);

    // State
    NoxView *focused_view() const { return m_focused; }
    int      active_workspace() const { return m_active_ws; }

    // Public wlroots handles (accessed by output/keyboard callbacks)
    struct wl_display           *display      = nullptr;
    struct wlr_backend          *backend      = nullptr;
    struct wlr_renderer         *renderer     = nullptr;
    struct wlr_allocator        *allocator    = nullptr;
    struct wlr_scene            *scene        = nullptr;
    struct wlr_scene_output_layout *scene_layout = nullptr;
    struct wlr_output_layout    *output_layout = nullptr;
    struct wlr_xdg_shell        *xdg_shell    = nullptr;
    struct wlr_seat             *seat         = nullptr;
    struct wlr_cursor           *cursor       = nullptr;
    struct wlr_xcursor_manager  *cursor_mgr   = nullptr;

    std::unique_ptr<NoxConfig>  config;

    // All views across all workspaces
    std::vector<NoxView *>      views;
    std::vector<NoxOutput *>    outputs;
    std::vector<NoxKeyboard *>  keyboards;

    // Workspaces (9 total, 1-indexed)
    static constexpr int MAX_WORKSPACES = 9;
    std::unique_ptr<Workspace>  workspaces[MAX_WORKSPACES];

private:
    NoxView *m_focused     = nullptr;
    int      m_active_ws   = 0; // index into workspaces[]

    // wl_listeners
    struct wl_listener m_new_output;
    struct wl_listener m_new_xdg_toplevel;
    struct wl_listener m_new_input;
    struct wl_listener m_cursor_motion;
    struct wl_listener m_cursor_motion_abs;
    struct wl_listener m_cursor_button;
    struct wl_listener m_cursor_axis;
    struct wl_listener m_cursor_frame;
    struct wl_listener m_request_cursor;

    void apply_layout();
    NoxView *view_at(double lx, double ly,
                     struct wlr_surface **surface,
                     double *sx, double *sy);
};
