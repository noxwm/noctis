#include "layout/workspace.hpp"
#include "core/server.hpp"  // NoxView

extern "C" {
#include <wlr/types/wlr_scene.h>
}

#include <algorithm>

Workspace::Workspace(int id, int sw, int sh, int gaps)
    : id(id)
{
    layout = std::make_unique<ColumnLayout>(sw, sh, gaps);
}

void Workspace::add_view(NoxView *view) {
    views.push_back(view);
    layout->add_view(view);
}

void Workspace::remove_view(NoxView *view) {
    views.erase(std::remove(views.begin(), views.end(), view), views.end());
    layout->remove_view(view);

    if (m_focused_idx >= (int)views.size() && m_focused_idx > 0)
        m_focused_idx--;
}

void Workspace::set_active(bool a) {
    active = a;
    for (auto *view : views) {
        wlr_scene_node_set_enabled(&view->scene_tree->node, a);
    }
}

void Workspace::focus_next() {
    if (views.empty()) return;
    m_focused_idx = (m_focused_idx + 1) % (int)views.size();
    layout->focus_next_column();
}

void Workspace::focus_prev() {
    if (views.empty()) return;
    m_focused_idx = (m_focused_idx - 1 + (int)views.size()) % (int)views.size();
    layout->focus_prev_column();
}

NoxView *Workspace::focused_view() const {
    if (views.empty()) return nullptr;
    return views[m_focused_idx];
}
