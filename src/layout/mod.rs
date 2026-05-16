use smithay::utils::{Logical, Rectangle, Size};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct WindowId(pub usize);

#[derive(Debug, Clone)]
pub struct WindowGeom {
    pub id:   WindowId,
    pub rect: Rectangle<i32, Logical>,
}

#[derive(Debug, Default, Clone)]
pub struct Column {
    pub windows: Vec<WindowId>,
}

#[derive(Debug)]
pub struct ColumnLayout {
    screen:          Size<i32, Logical>,
    gaps:            i32,
    pub columns:     Vec<Column>,
    pub focused_col: usize,
    scroll_x:        i32,
}

impl ColumnLayout {
    pub fn new(screen: Size<i32, Logical>, gaps: i32) -> Self {
        ColumnLayout { screen, gaps, columns: vec![], focused_col: 0, scroll_x: 0 }
    }

    pub fn update_screen(&mut self, screen: Size<i32, Logical>) {
        self.screen = screen;
    }

    pub fn add_window(&mut self, id: WindowId) {
        self.columns.push(Column { windows: vec![id] });
        self.focused_col = self.columns.len().saturating_sub(1);
        self.scroll_to_focused();
    }

    pub fn remove_window(&mut self, id: WindowId) {
        for col in &mut self.columns {
            col.windows.retain(|w| *w != id);
        }
        self.columns.retain(|c| !c.windows.is_empty());
        if self.focused_col >= self.columns.len() && !self.columns.is_empty() {
            self.focused_col = self.columns.len() - 1;
        }
        self.scroll_to_focused();
    }

    pub fn focus_next(&mut self) {
        if self.columns.is_empty() { return; }
        self.focused_col = (self.focused_col + 1).min(self.columns.len() - 1);
        self.scroll_to_focused();
    }

    pub fn focus_prev(&mut self) {
        if self.columns.is_empty() { return; }
        self.focused_col = self.focused_col.saturating_sub(1);
        self.scroll_to_focused();
    }

    pub fn set_focused_by_id(&mut self, id: WindowId) {
        for (ci, col) in self.columns.iter().enumerate() {
            if col.windows.contains(&id) {
                self.focused_col = ci;
                self.scroll_to_focused();
                return;
            }
        }
    }

    pub fn focused_window(&self) -> Option<WindowId> {
        self.columns.get(self.focused_col)?.windows.first().copied()
    }

    pub fn arrange(&self) -> Vec<WindowGeom> {
        if self.columns.is_empty() || self.screen.w == 0 { return vec![]; }

        let n     = self.columns.len() as i32;
        let gaps  = self.gaps;
        let col_w = ((self.screen.w - gaps * (n + 1)) / n).max(100);
        let mut result = Vec::new();
        let mut x = gaps;

        for col in &self.columns {
            let nv    = col.windows.len() as i32;
            let row_h = ((self.screen.h - gaps * (nv + 1)) / nv).max(50);
            let mut y = gaps;

            for &wid in &col.windows {
                result.push(WindowGeom {
                    id:   wid,
                    rect: Rectangle::new(
                        (x - self.scroll_x, y).into(),
                        (col_w, row_h).into(),
                    ),
                });
                y += row_h + gaps;
            }
            x += col_w + gaps;
        }
        result
    }

    fn scroll_to_focused(&mut self) {
        if self.columns.is_empty() || self.screen.w == 0 { return; }
        let n     = self.columns.len() as i32;
        let gaps  = self.gaps;
        let col_w = ((self.screen.w - gaps * (n + 1)) / n).max(100);
        let col_abs_x = gaps + self.focused_col as i32 * (col_w + gaps);
        let col_right = col_abs_x + col_w;
        if col_abs_x - self.scroll_x < gaps {
            self.scroll_x = col_abs_x - gaps;
        } else if col_right - self.scroll_x > self.screen.w - gaps {
            self.scroll_x = col_right - (self.screen.w - gaps);
        }
    }
}

pub struct Workspace {
    pub id:     usize,
    pub layout: ColumnLayout,
}

impl Workspace {
    pub fn new(id: usize, screen: Size<i32, Logical>, gaps: i32) -> Self {
        Workspace { id, layout: ColumnLayout::new(screen, gaps) }
    }
}
