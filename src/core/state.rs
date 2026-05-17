use std::collections::HashMap;

use smithay::{
    delegate_compositor, delegate_data_device, delegate_output,
    delegate_seat, delegate_shm, delegate_xdg_shell,
    desktop::{PopupManager, Space, Window},
    input::{
        keyboard::{KeyboardHandle, XkbConfig},
        pointer::{CursorImageStatus, PointerHandle},
        Seat, SeatHandler, SeatState,
    },
    output::{Output},
    reexports::{
        calloop::LoopSignal,
        wayland_server::{
            backend::{ClientData, ClientId, DisconnectReason},
            protocol::wl_surface::WlSurface,
            Display,
        },
    },
    utils::{Logical, Point, Size, SERIAL_COUNTER},
    wayland::{
        buffer::BufferHandler,
        compositor::{CompositorClientState, CompositorHandler, CompositorState},
        output::OutputManagerState,
        selection::{
            data_device::{
                ClientDndGrabHandler, DataDeviceHandler, DataDeviceState,
                ServerDndGrabHandler,
            },
            SelectionHandler,
        },
        shell::xdg::{
            PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState,
        },
        shm::{ShmHandler, ShmState},
    },
};

use crate::config::NoxConfig;
use crate::layout::{WindowId, Workspace};

// ── ClientState ───────────────────────────────────────────────────────────────

#[derive(Default)]
pub struct ClientState {
    pub compositor_state: CompositorClientState,
}

impl ClientData for ClientState {
    fn initialized(&self, _id: ClientId) {}
    fn disconnected(&self, _id: ClientId, _reason: DisconnectReason) {}
}

// ── NoxState ──────────────────────────────────────────────────────────────────

pub struct NoxState {
    pub compositor_state:  CompositorState,
    pub xdg_shell_state:   XdgShellState,
    pub shm_state:         ShmState,
    pub output_mgr_state:  OutputManagerState,
    pub seat_state:        SeatState<Self>,
    pub data_device_state: DataDeviceState,
    pub popup_manager:     PopupManager,

    pub seat:     Seat<Self>,
    pub keyboard: KeyboardHandle<Self>,
    pub pointer:  PointerHandle<Self>,

    pub space:  Space<Window>,
    pub output: Option<Output>,

    pub workspaces:   Vec<Workspace>,
    pub active_ws:    usize,
    pub window_to_ws: HashMap<WindowId, usize>,
    pub next_id:      usize,
    pub window_ids:   HashMap<Window, WindowId>,

    pub config:        NoxConfig,
    pub loop_signal:   LoopSignal,
    pub cursor_status: CursorImageStatus,

    // track whether we should quit
    pub should_quit: bool,
}

impl NoxState {
    pub fn new(display: &Display<Self>, config: NoxConfig, loop_signal: LoopSignal) -> Self {
        let dh = display.handle();

        let compositor_state  = CompositorState::new::<Self>(&dh);
        let xdg_shell_state   = XdgShellState::new::<Self>(&dh);
        let shm_state         = ShmState::new::<Self>(&dh, vec![]);
        let output_mgr_state  = OutputManagerState::new_with_xdg_output::<Self>(&dh);
        let mut seat_state    = SeatState::new();
        let data_device_state = DataDeviceState::new::<Self>(&dh);

        let mut seat = seat_state.new_wl_seat(&dh, "seat0");
        let keyboard = seat.add_keyboard(XkbConfig::default(), 200, 25).unwrap();
        let pointer  = seat.add_pointer();

        let dummy = Size::from((1920, 1080));
        let workspaces = (0..9).map(|i| Workspace::new(i, dummy, config.gaps)).collect();

        NoxState {
            compositor_state, xdg_shell_state, shm_state,
            output_mgr_state, seat_state, data_device_state,
            popup_manager: PopupManager::default(),
            seat, keyboard, pointer,
            space: Space::default(),
            output: None,
            workspaces, active_ws: 0,
            window_to_ws: HashMap::new(),
            next_id: 0,
            window_ids: HashMap::new(),
            config, loop_signal,
            cursor_status: CursorImageStatus::default_named(),
            should_quit: false,
        }
    }

    pub fn screen_size(&self) -> Size<i32, Logical> {
        self.output.as_ref()
            .and_then(|o| o.current_mode().map(|m| m.size.to_logical(1)))
            .unwrap_or_else(|| Size::from((1920, 1080)))
    }

    pub fn apply_layout(&mut self) {
        let screen = self.screen_size();
        self.workspaces[self.active_ws].layout.update_screen(screen);
        let geoms: Vec<_> = self.workspaces[self.active_ws].layout.arrange();
        for wg in geoms {
            if let Some(win) = self.find_window(wg.id) {
                self.space.map_element(win.clone(), wg.rect.loc, false);
                win.toplevel().unwrap().with_pending_state(|s| { s.size = Some(wg.rect.size); });
                win.toplevel().unwrap().send_configure();
            }
        }
    }

    pub fn find_window(&self, id: WindowId) -> Option<Window> {
        self.window_ids.iter()
            .find_map(|(w, &wid)| if wid == id { Some(w.clone()) } else { None })
    }

    pub fn focused_window(&self) -> Option<Window> {
        let id = self.workspaces[self.active_ws].layout.focused_window()?;
        self.find_window(id)
    }

    // Set keyboard focus — avoids the borrow conflict by cloning the surface first
    pub fn focus_surface(&mut self, surface: Option<WlSurface>) {
        let serial = SERIAL_COUNTER.next_serial();
        // We need to temporarily take keyboard out of self to avoid borrow conflict
        let kb = self.keyboard.clone();
        kb.set_focus(self, surface, serial);
    }

    pub fn focus_window(&mut self, win: Option<Window>) {
        let surface = win.as_ref()
            .and_then(|w| w.toplevel())
            .map(|tl| tl.wl_surface().clone());
        if let Some(w) = &win {
            self.space.raise_element(w, true);
        }
        self.focus_surface(surface);
    }

    pub fn switch_workspace(&mut self, idx: usize) {
        if idx == self.active_ws || idx >= self.workspaces.len() { return; }

        // unmap current
        let cur: Vec<Window> = self.workspaces[self.active_ws].layout.arrange()
            .iter().filter_map(|g| self.find_window(g.id)).collect();
        for w in cur { self.space.unmap_elem(&w); }

        self.active_ws = idx;
        let screen = self.screen_size();
        self.workspaces[self.active_ws].layout.update_screen(screen);
        let geoms: Vec<_> = self.workspaces[self.active_ws].layout.arrange();
        for wg in geoms {
            if let Some(win) = self.find_window(wg.id) {
                self.space.map_element(win.clone(), wg.rect.loc, false);
                win.toplevel().unwrap().with_pending_state(|s| { s.size = Some(wg.rect.size); });
                win.toplevel().unwrap().send_configure();
            }
        }

        let focused = self.focused_window();
        self.focus_window(focused);
    }

    pub fn click_focus_at(&mut self, pos: Point<f64, Logical>) {
        if let Some((win, _)) = self.space.element_under(pos).map(|(w, l)| (w.clone(), l)) {
            if let Some(&id) = self.window_ids.get(&win) {
                self.workspaces[self.active_ws].layout.set_focused_by_id(id);
            }
            self.focus_window(Some(win));
        }
    }
}

// ── Protocol delegates ────────────────────────────────────────────────────────

impl BufferHandler for NoxState {
    fn buffer_destroyed(&mut self, _buffer: &smithay::reexports::wayland_server::protocol::wl_buffer::WlBuffer) {}
}

impl CompositorHandler for NoxState {
    fn compositor_state(&mut self) -> &mut CompositorState { &mut self.compositor_state }
    fn client_compositor_state<'a>(&self, client: &'a smithay::reexports::wayland_server::Client)
        -> &'a CompositorClientState
    {
        &client.get_data::<ClientState>().unwrap().compositor_state
    }
    fn commit(&mut self, surface: &WlSurface) {
        self.popup_manager.commit(surface);
    }
}
delegate_compositor!(NoxState);

impl XdgShellHandler for NoxState {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState { &mut self.xdg_shell_state }

    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        let window = Window::new_wayland_window(surface);
        let id     = WindowId(self.next_id);
        self.next_id += 1;
        let ws = self.active_ws;
        self.workspaces[ws].layout.add_window(id);
        self.window_to_ws.insert(id, ws);
        self.window_ids.insert(window.clone(), id);
        self.apply_layout();
        let focused = self.focused_window();
        self.focus_window(focused);
    }

    fn toplevel_destroyed(&mut self, surface: ToplevelSurface) {
        let win = self.window_ids.keys().find(|w| {
            w.toplevel().map_or(false, |t| t.wl_surface() == surface.wl_surface())
        }).cloned();

        if let Some(win) = win {
            if let Some(&id) = self.window_ids.get(&win) {
                if let Some(&ws) = self.window_to_ws.get(&id) {
                    self.workspaces[ws].layout.remove_window(id);
                }
                self.window_to_ws.remove(&id);
                self.window_ids.remove(&win);
                self.space.unmap_elem(&win);
            }
            self.apply_layout();
            let focused = self.focused_window();
            self.focus_window(focused);
        }
    }

    fn new_popup(&mut self, surface: PopupSurface, _positioner: PositionerState) {
        self.popup_manager.track_popup(surface.into()).ok();
    }
    fn grab(&mut self, _surface: PopupSurface,
            _seat: smithay::reexports::wayland_server::protocol::wl_seat::WlSeat,
            _serial: smithay::utils::Serial) {}
    fn reposition_request(&mut self, _surface: PopupSurface,
            _positioner: PositionerState, _token: u32) {}
}
delegate_xdg_shell!(NoxState);

impl ShmHandler for NoxState {
    fn shm_state(&self) -> &ShmState { &self.shm_state }
}
delegate_shm!(NoxState);

impl SeatHandler for NoxState {
    type KeyboardFocus = WlSurface;
    type PointerFocus  = WlSurface;
    type TouchFocus    = WlSurface;

    fn seat_state(&mut self) -> &mut SeatState<Self> { &mut self.seat_state }
    fn cursor_image(&mut self, _seat: &Seat<Self>, image: CursorImageStatus) {
        self.cursor_status = image;
    }
    fn focus_changed(&mut self, _seat: &Seat<Self>, _focused: Option<&WlSurface>) {}
}
delegate_seat!(NoxState);

impl DataDeviceHandler for NoxState {
    fn data_device_state(&self) -> &DataDeviceState { &self.data_device_state }
}
impl ClientDndGrabHandler for NoxState {}
impl ServerDndGrabHandler for NoxState {}
impl SelectionHandler for NoxState {
    type SelectionUserData = ();
}
delegate_data_device!(NoxState);

// OutputHandler is just a marker trait in 0.7
impl smithay::wayland::output::OutputHandler for NoxState {}
delegate_output!(NoxState);
