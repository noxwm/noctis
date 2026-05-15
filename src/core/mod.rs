use std::collections::HashMap;
use std::process::Command;
use std::sync::Arc;
use std::time::Duration;

use smithay::{
    backend::winit::{self, WinitEvent},
    delegate_compositor, delegate_data_device, delegate_output,
    delegate_seat, delegate_shm, delegate_xdg_shell,
    desktop::{Space, Window},
    input::{
        keyboard::{keysyms, FilterResult, KeyboardHandle},
        pointer::{
            ButtonEvent, CursorImageStatus, MotionEvent, PointerHandle,
        },
        Seat, SeatHandler, SeatState,
    },
    output::{Mode, Output, PhysicalProperties, Subpixel},
    reexports::{
        calloop::{EventLoop, LoopSignal},
        wayland_server::{
            backend::{ClientData, ClientId, DisconnectReason},
            protocol::wl_surface::WlSurface,
            Display,
        },
    },
    utils::{Logical, Physical, Point, Rectangle, Size, Transform, SERIAL_COUNTER},
    wayland::{
        compositor::{CompositorClientState, CompositorHandler, CompositorState},
        data_device::{
            ClientDndGrabHandler, DataDeviceHandler, DataDeviceState, ServerDndGrabHandler,
        },
        output::OutputManagerState,
        selection::SelectionHandler,
        shell::xdg::{
            PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState,
            XdgToplevelSurfaceData,
        },
        shm::{ShmHandler, ShmState},
    },
};

use crate::config::{Action, NoxConfig};
use crate::input::match_keybind;
use crate::layout::{ColumnLayout, WindowId, Workspace};

// ── Per-client state ──────────────────────────────────────────────────────────

#[derive(Default)]
pub struct ClientState {
    pub compositor_state: CompositorClientState,
}

impl ClientData for ClientState {
    fn initialized(&self, _client_id: ClientId) {}
    fn disconnected(&self, _client_id: ClientId, _reason: DisconnectReason) {}
}

// ── NoxState ──────────────────────────────────────────────────────────────────

pub struct NoxState {
    // Core smithay state
    pub compositor_state:  CompositorState,
    pub xdg_shell_state:   XdgShellState,
    pub shm_state:         ShmState,
    pub output_mgr_state:  OutputManagerState,
    pub seat_state:        SeatState<Self>,
    pub data_device_state: DataDeviceState,

    // Input
    pub seat:     Seat<Self>,
    pub keyboard: KeyboardHandle<Self>,
    pub pointer:  PointerHandle<Self>,

    // Display
    pub space:  Space<Window>,
    pub output: Option<Output>,

    // Layout
    pub workspaces:     Vec<Workspace>,
    pub active_ws:      usize,
    pub window_to_ws:   HashMap<WindowId, usize>,
    pub next_window_id: usize,
    pub window_ids:     HashMap<Window, WindowId>,

    // Config
    pub config: NoxConfig,

    // Event loop signal for quit
    pub loop_signal: LoopSignal,
}

impl NoxState {
    fn new(
        display:     &Display<Self>,
        config:      NoxConfig,
        loop_signal: LoopSignal,
    ) -> Self {
        let dh = display.handle();

        let compositor_state  = CompositorState::new::<Self>(&dh);
        let xdg_shell_state   = XdgShellState::new::<Self>(&dh);
        let shm_state         = ShmState::new::<Self>(&dh, vec![]);
        let output_mgr_state  = OutputManagerState::new_with_xdg_output::<Self>(&dh);
        let mut seat_state    = SeatState::new();
        let data_device_state = DataDeviceState::new::<Self>(&dh);

        let mut seat = seat_state.new_wl_seat(&dh, "seat0");
        let keyboard = seat.add_keyboard(Default::default(), 200, 25).unwrap();
        let pointer  = seat.add_pointer();

        // 9 workspaces with dummy screen size (updated when output is added)
        let dummy_screen = Size::from((1920, 1080));
        let workspaces: Vec<Workspace> = (0..9)
            .map(|i| Workspace::new(i, dummy_screen, config.gaps))
            .collect();

        NoxState {
            compositor_state,
            xdg_shell_state,
            shm_state,
            output_mgr_state,
            seat_state,
            data_device_state,
            seat,
            keyboard,
            pointer,
            space:          Space::default(),
            output:         None,
            workspaces,
            active_ws:      0,
            window_to_ws:   HashMap::new(),
            next_window_id: 0,
            window_ids:     HashMap::new(),
            config,
            loop_signal,
        }
    }

    // ── Layout helpers ────────────────────────────────────────────────────────

    fn screen_size(&self) -> Size<i32, Logical> {
        self.output
            .as_ref()
            .map(|o| o.current_mode().map_or(
                Size::from((1920, 1080)),
                |m| m.size.to_logical(1),
            ))
            .unwrap_or_else(|| Size::from((1920, 1080)))
    }

    fn apply_layout(&mut self) {
        let screen = self.screen_size();
        let ws     = &mut self.workspaces[self.active_ws];
        ws.layout.update_screen(screen);

        let geoms: Vec<_> = ws.layout.arrange();

        for wg in geoms {
            // Find the Window that has this WindowId
            if let Some(win) = self.window_ids.iter()
                .find_map(|(w, &id)| if id == wg.id { Some(w.clone()) } else { None })
            {
                self.space.map_element(win.clone(), wg.rect.loc, false);
                win.toplevel().unwrap().with_pending_state(|s| {
                    s.size = Some(wg.rect.size);
                });
                win.toplevel().unwrap().send_configure();
            }
        }
    }

    fn focused_window(&self) -> Option<Window> {
        let id = self.workspaces[self.active_ws].layout.focused_window()?;
        self.window_ids.iter()
            .find_map(|(w, &wid)| if wid == id { Some(w.clone()) } else { None })
    }

    fn focus_window(&mut self, win: Option<Window>) {
        let serial = SERIAL_COUNTER.next_serial();
        if let Some(w) = &win {
            let surface = w.toplevel().unwrap().wl_surface().clone();
            self.keyboard.set_focus(self, Some(surface.into()), serial);
            self.space.raise_element(w, true);
        } else {
            self.keyboard.set_focus(self, None, serial);
        }
    }

    // ── Workspace switch ──────────────────────────────────────────────────────

    fn switch_workspace(&mut self, idx: usize) {
        if idx == self.active_ws || idx >= self.workspaces.len() { return; }

        // Hide current workspace windows
        let current_ids: Vec<WindowId> = self.workspaces[self.active_ws]
            .layout.arrange().iter().map(|g| g.id).collect();

        for wid in current_ids {
            if let Some(win) = self.window_ids.iter()
                .find_map(|(w, &id)| if id == wid { Some(w.clone()) } else { None })
            {
                self.space.unmap_elem(&win);
            }
        }

        self.active_ws = idx;

        // Show new workspace windows
        let screen = self.screen_size();
        self.workspaces[self.active_ws].layout.update_screen(screen);
        let geoms: Vec<_> = self.workspaces[self.active_ws].layout.arrange();

        for wg in geoms {
            if let Some(win) = self.window_ids.iter()
                .find_map(|(w, &id)| if id == wg.id { Some(w.clone()) } else { None })
            {
                self.space.map_element(win.clone(), wg.rect.loc, false);
                win.toplevel().unwrap().with_pending_state(|s| {
                    s.size = Some(wg.rect.size);
                });
                win.toplevel().unwrap().send_configure();
            }
        }

        let focused = self.focused_window();
        self.focus_window(focused);
    }
}

// ── Smithay protocol delegates ────────────────────────────────────────────────

impl CompositorHandler for NoxState {
    fn compositor_state(&mut self) -> &mut CompositorState {
        &mut self.compositor_state
    }
    fn client_compositor_state<'a>(&self, client: &'a smithay::reexports::wayland_server::Client)
        -> &'a CompositorClientState
    {
        &client.get_data::<ClientState>().unwrap().compositor_state
    }
    fn commit(&mut self, surface: &WlSurface) {
        smithay::desktop::utils::on_commit_buffer_handler::<Self>(surface);
    }
}
delegate_compositor!(NoxState);

impl XdgShellHandler for NoxState {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.xdg_shell_state
    }

    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        let window = Window::new_wayland_window(surface);

        let id = WindowId(self.next_window_id);
        self.next_window_id += 1;

        let ws_idx = self.active_ws;
        self.workspaces[ws_idx].layout.add_window(id);
        self.window_to_ws.insert(id, ws_idx);
        self.window_ids.insert(window.clone(), id);

        self.apply_layout();

        let focused = self.focused_window();
        self.focus_window(focused);
    }

    fn toplevel_destroyed(&mut self, surface: ToplevelSurface) {
        let win = self.window_ids.keys()
            .find(|w| w.toplevel().map_or(false, |t| t.wl_surface() == surface.wl_surface()))
            .cloned();

        if let Some(win) = win {
            if let Some(&id) = self.window_ids.get(&win) {
                if let Some(&ws_idx) = self.window_to_ws.get(&id) {
                    self.workspaces[ws_idx].layout.remove_window(id);
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

    fn new_popup(&mut self, _surface: PopupSurface, _positioner: PositionerState) {}
    fn grab(&mut self, _surface: PopupSurface, _seat: smithay::reexports::wayland_server::protocol::wl_seat::WlSeat, _serial: smithay::utils::Serial) {}
    fn reposition_request(&mut self, _surface: PopupSurface, _positioner: PositionerState, _token: u32) {}
}
delegate_xdg_shell!(NoxState);

impl ShmHandler for NoxState {
    fn shm_state(&self) -> &ShmState { &self.shm_state }
}
delegate_shm!(NoxState);

impl SeatHandler for NoxState {
    type KeyboardFocus = smithay::input::keyboard::KeyboardFocus;
    type PointerFocus  = smithay::desktop::Window;
    type TouchFocus    = smithay::desktop::Window;

    fn seat_state(&mut self) -> &mut SeatState<Self> { &mut self.seat_state }

    fn cursor_image(&mut self, _seat: &Seat<Self>, _image: CursorImageStatus) {}
    fn focus_changed(&mut self, _seat: &Seat<Self>, _focused: &Option<Self::KeyboardFocus>) {}
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
delegate_output!(NoxState);

// ── run() ─────────────────────────────────────────────────────────────────────

pub fn run(config: NoxConfig) {
    let mut event_loop: EventLoop<NoxState> = EventLoop::try_new().unwrap();
    let mut display: Display<NoxState>      = Display::new().unwrap();

    let loop_signal = event_loop.get_signal();

    let mut state = NoxState::new(&display, config, loop_signal);

    // ── Winit backend (runs inside an existing session for testing) ───────────
    let (mut winit_backend, mut winit_evt_loop) =
        winit::init::<smithay::backend::renderer::gles::GlesRenderer>()
            .expect("failed to init winit backend — are you in a graphical session?");

    let mode = Mode {
        size:    winit_backend.window_size(),
        refresh: 60_000,
    };

    let output = Output::new(
        "winit-0".into(),
        PhysicalProperties {
            size:     (0, 0).into(),
            subpixel: Subpixel::Unknown,
            make:     "noxwm".into(),
            model:    "winit".into(),
        },
    );
    output.change_current_state(Some(mode), Some(Transform::Flipped180), None, Some((0, 0).into()));
    output.set_preferred(mode);

    state.space.map_output(&output, (0, 0));
    state.output = Some(output.clone());

    // Update workspace screen sizes now that we know the output
    let screen = Size::from((mode.size.w, mode.size.h));
    for ws in &mut state.workspaces {
        ws.layout.update_screen(screen.to_logical(1));
    }

    // Set WAYLAND_DISPLAY so clients can connect
    let socket_name = display.add_socket_auto().unwrap();
    std::env::set_var("WAYLAND_DISPLAY", &socket_name);
    tracing::info!("WAYLAND_DISPLAY={}", socket_name.to_string_lossy());

    // Launch autostart
    let autostart = state.config.autostart.clone();
    for cmd in &autostart {
        tracing::info!("autostart: {cmd}");
        let _ = Command::new("sh").args(["-c", cmd]).spawn();
    }

    // ── Event loop ────────────────────────────────────────────────────────────
    let loop_handle = event_loop.handle();

    loop_handle.insert_source(
        smithay::reexports::calloop::generic::Generic::new(
            display.backend().poll_fd().try_clone_to_owned().unwrap(),
            smithay::reexports::calloop::Interest::READ,
            smithay::reexports::calloop::Mode::Level,
        ),
        |_, _, state: &mut NoxState| {
            state.space.refresh();
            Ok(smithay::reexports::calloop::PostAction::Continue)
        },
    ).unwrap();

    loop {
        // Process winit events
        winit_evt_loop.dispatch_new_events(|event| {
            match event {
                WinitEvent::Resized { size, .. } => {
                    let screen = size.to_logical(1);
                    for ws in &mut state.workspaces {
                        ws.layout.update_screen(screen);
                    }
                    state.apply_layout();
                }

                WinitEvent::Input(input_event) => {
                    use smithay::backend::input::InputEvent;
                    match input_event {
                        InputEvent::Keyboard { event } => {
                            use smithay::backend::input::KeyboardKeyEvent;
                            let keycode = event.key_code();
                            let key_state = event.state();

                            let serial = SERIAL_COUNTER.next_serial();
                            let time   = event.time_msec();

                            state.keyboard.input::<(), _>(
                                &mut state,
                                keycode,
                                key_state,
                                serial,
                                time,
                                |nox_state, mods, handle| {
                                    if key_state != smithay::backend::input::KeyState::Pressed {
                                        return FilterResult::Forward;
                                    }

                                    // Get key name from keysym
                                    let sym = handle.modified_sym();
                                    let key_name = keysym_to_name(sym);

                                    let keybinds = nox_state.config.keybinds.clone();
                                    if let Some(action) = match_keybind(&keybinds, &key_name, mods) {
                                        match action.clone() {
                                            Action::Exec(cmd) => {
                                                let _ = Command::new("sh").args(["-c", &cmd]).spawn();
                                            }
                                            Action::Close => {
                                                if let Some(win) = nox_state.focused_window() {
                                                    if let Some(tl) = win.toplevel() {
                                                        tl.send_close();
                                                    }
                                                }
                                            }
                                            Action::FocusLeft => {
                                                nox_state.workspaces[nox_state.active_ws]
                                                    .layout.focus_prev();
                                                let focused = nox_state.focused_window();
                                                nox_state.focus_window(focused);
                                            }
                                            Action::FocusRight => {
                                                nox_state.workspaces[nox_state.active_ws]
                                                    .layout.focus_next();
                                                let focused = nox_state.focused_window();
                                                nox_state.focus_window(focused);
                                            }
                                            Action::Fullscreen => {
                                                // v1: skip, add in v2
                                            }
                                            Action::Workspace(idx) => {
                                                nox_state.switch_workspace(idx);
                                            }
                                            Action::Quit => {
                                                nox_state.loop_signal.stop();
                                            }
                                        }
                                        return FilterResult::Intercept(());
                                    }

                                    FilterResult::Forward
                                },
                            );
                        }

                        InputEvent::PointerMotion { event } => {
                            use smithay::backend::input::PointerMotionEvent;
                            let serial = SERIAL_COUNTER.next_serial();
                            let pos    = state.pointer.current_location()
                                + event.delta();
                            state.pointer.motion(
                                &mut state,
                                None,
                                &MotionEvent { location: pos, serial, time: event.time_msec() },
                            );
                        }

                        InputEvent::PointerButton { event } => {
                            use smithay::backend::input::PointerButtonEvent;
                            let serial = SERIAL_COUNTER.next_serial();
                            let pos    = state.pointer.current_location();

                            // Click to focus
                            if let Some((win, _)) = state.space
                                .element_under(pos)
                                .map(|(w, p)| (w.clone(), p))
                            {
                                let id = state.window_ids.get(&win).copied();
                                if let Some(id) = id {
                                    // Update layout focus
                                    let ws = &mut state.workspaces[state.active_ws];
                                    // find which column this window is in
                                    for (ci, col) in ws.layout.columns.iter().enumerate() {
                                        if col.windows.contains(&id) {
                                            ws.layout.focused_col = ci;
                                            break;
                                        }
                                    }
                                }
                                state.focus_window(Some(win));
                            }

                            state.pointer.button(
                                &mut state,
                                &ButtonEvent {
                                    serial,
                                    time:   event.time_msec(),
                                    button: event.button_code(),
                                    state:  event.state(),
                                },
                            );
                        }

                        _ => {}
                    }
                }

                WinitEvent::CloseRequested => {
                    state.loop_signal.stop();
                }

                _ => {}
            }
        });

        // Render
        winit_backend.bind().unwrap();
        let age = winit_backend.buffer_age().unwrap_or(0);

        let renderer = winit_backend.renderer();
        let elements = smithay::desktop::space::render_elements_from_surface_tree(
            renderer,
            state.space.elements(),
            (0, 0),
            1.0,
            1.0,
            smithay::backend::renderer::element::Kind::Unspecified,
        );

        renderer.render(
            mode.size,
            Transform::Flipped180,
            &elements,
            [0.1, 0.1, 0.1, 1.0],
        ).unwrap();

        winit_backend.submit(None).unwrap();

        // Process Wayland client requests
        display.dispatch_clients(&mut state).unwrap();
        display.flush_clients().unwrap();

        event_loop
            .dispatch(Some(Duration::from_millis(1)), &mut state)
            .unwrap();

        if state.loop_signal.is_stopped() { break; }
    }
}

// ── keysym → name ─────────────────────────────────────────────────────────────

fn keysym_to_name(sym: u32) -> String {
    // Handle common keys directly
    match sym {
        keysyms::KEY_Return | keysyms::KEY_KP_Enter => "Return".into(),
        keysyms::KEY_space        => "space".into(),
        keysyms::KEY_Tab          => "Tab".into(),
        keysyms::KEY_Escape       => "Escape".into(),
        keysyms::KEY_BackSpace     => "BackSpace".into(),
        keysyms::KEY_Delete        => "Delete".into(),
        keysyms::KEY_Left          => "Left".into(),
        keysyms::KEY_Right         => "Right".into(),
        keysyms::KEY_Up            => "Up".into(),
        keysyms::KEY_Down          => "Down".into(),
        keysyms::KEY_F1..=keysyms::KEY_F12 => {
            format!("F{}", sym - keysyms::KEY_F1 + 1)
        }
        // a-z
        s if s >= keysyms::KEY_a && s <= keysyms::KEY_z => {
            char::from_u32(s).map(|c| c.to_string()).unwrap_or_default()
        }
        // A-Z → lowercase for matching
        s if s >= keysyms::KEY_A && s <= keysyms::KEY_Z => {
            char::from_u32(s).map(|c| c.to_ascii_lowercase().to_string()).unwrap_or_default()
        }
        // 0-9
        s if s >= keysyms::KEY_0 && s <= keysyms::KEY_9 => {
            char::from_u32(s).map(|c| c.to_string()).unwrap_or_default()
        }
        _ => format!("{sym:#x}"),
    }
}
