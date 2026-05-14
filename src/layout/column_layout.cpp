#include "layout/column_layout.hpp"
#include "core/server.hpp"  // for NoxView

#include <algorithm>
#include <cmath>

ColumnLayout::ColumnLayout(int screen_w, int screen_h, int gaps)
    : m_screen_w(screen_w), m_screen_h(screen_h), m_gaps(gaps)
{}

void ColumnLayout::add_view(NoxView *view) {
    place_view(view);
    recalculate();
}

void ColumnLayout::remove_view(NoxView *view) {
    for (auto it = columns.begin(); it != columns.end(); ++it) {
        auto &col = *it;
        auto vit = std::find(col.views.begin(), col.views.end(), view);
        if (vit == col.views.end()) continue;

        col.views.erase(vit);
        if (col.views.empty()) {
            columns.erase(it);
            if (m_focused_col >= (int)columns.size() && m_focused_col > 0)
                m_focused_col--;
        }
        recalculate();
        return;
    }
}

void ColumnLayout::place_view(NoxView *view) {
    // Strategy: each new window gets its own column.
    // This matches niri's default behaviour.
    Column col;
    col.views.push_back(view);
    columns.push_back(col);
    m_focused_col = (int)columns.size() - 1;
}

void ColumnLayout::recalculate() {
    if (columns.empty() || m_screen_w == 0) return;

    int n = (int)columns.size();

    // Each column gets an equal share of the screen width
    int col_w = (m_screen_w - m_gaps * (n + 1)) / n;
    col_w = std::max(col_w, 100); // minimum column width

    int x = m_gaps;
    for (auto &col : columns) {
        col.x     = x - m_scroll_x;
        col.width = col_w;

        int nv = (int)col.views.size();
        int row_h = (m_screen_h - m_gaps * (nv + 1)) / nv;
        int y = m_gaps;

        for (auto *view : col.views) {
            view->x      = col.x;
            view->y      = y;
            view->width  = col_w;
            view->height = row_h;
            y += row_h + m_gaps;
        }

        x += col_w + m_gaps;
    }
}

void ColumnLayout::scroll_to(int col_idx) {
    if (col_idx < 0 || col_idx >= (int)columns.size()) return;

    int col_x     = columns[col_idx].x + m_scroll_x; // absolute x
    int col_right = col_x + columns[col_idx].width;

    if (col_x < m_gaps) {
        m_scroll_x -= (m_gaps - col_x);
    } else if (col_right > m_screen_w - m_gaps) {
        m_scroll_x += (col_right - (m_screen_w - m_gaps));
    }

    recalculate();
}

void ColumnLayout::focus_next_column() {
    if (columns.empty()) return;
    m_focused_col = std::min(m_focused_col + 1, (int)columns.size() - 1);
    scroll_to(m_focused_col);
}

void ColumnLayout::focus_prev_column() {
    if (columns.empty()) return;
    m_focused_col = std::max(m_focused_col - 1, 0);
    scroll_to(m_focused_col);
}
