#pragma once

#include <memory>
#include <string>
#include <vector>
#include "layout/column_layout.hpp"

struct NoxView;

class Workspace {
public:
    Workspace(int id, int screen_w, int screen_h, int gaps);

    void add_view(NoxView *view);
    void remove_view(NoxView *view);

    // Show/hide all views in this workspace
    void set_active(bool active);

    void focus_next();
    void focus_prev();

    NoxView *focused_view() const;

    int  id;
    bool active = false;

    std::unique_ptr<ColumnLayout> layout;
    std::vector<NoxView *>        views;

private:
    int m_focused_idx = 0;
};
