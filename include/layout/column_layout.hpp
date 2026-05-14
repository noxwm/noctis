#pragma once

#include <vector>

// Forward declare
struct NoxView;

// A single column in the layout — holds one or more windows stacked vertically
struct Column {
    std::vector<NoxView *> views;
    int x      = 0;  // absolute x position on screen
    int width  = 0;
};

// ColumnLayout manages the niri-style scrollable column layout for one workspace.
// Windows are arranged in columns, columns scroll horizontally.
class ColumnLayout {
public:
    ColumnLayout(int screen_w, int screen_h, int gaps);

    void add_view(NoxView *view);
    void remove_view(NoxView *view);
    void recalculate();   // recompute all positions and sizes

    // Scroll so that the focused column is visible
    void scroll_to(int column_index);

    int focused_column() const { return m_focused_col; }
    void focus_next_column();
    void focus_prev_column();

    std::vector<Column> columns;

private:
    int m_screen_w;
    int m_screen_h;
    int m_gaps;
    int m_scroll_x   = 0;
    int m_focused_col = 0;

    // Put a new window into an existing or new column
    void place_view(NoxView *view);
};
