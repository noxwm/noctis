#include "seat.hpp"
#include "server.hpp"

extern "C" {
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_compositor.h>
}

struct wlr_surface *desktop_surface_at(
    Server *server, double lx, double ly, double *sx, double *sy)
{
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);

    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return nullptr;

    struct wlr_scene_buffer *scene_buffer =
        wlr_scene_buffer_from_node(node);

    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);

    if (!scene_surface)
        return nullptr;

    return scene_surface->surface;
}
