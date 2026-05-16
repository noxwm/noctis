use std::collections::HashMap;
use std::process::Command;
use std::time::Duration;

use smithay::wayland::socket::ListeningSocketSource;
use smithay::utils::IsAlive;
use smithay::backend::renderer::element::Element;
use smithay::{
    backend::{
        input::{
            Event, InputEvent, KeyState, KeyboardKeyEvent,
            PointerButtonEvent, PointerMotionEvent,
        },
        winit::WinitInput,
        renderer::{
            damage::OutputDamageTracker,
            element::surface::{
                render_elements_from_surface_tree, WaylandSurfaceRenderElement,
            },
            gles::GlesRenderer,
        },
        winit::{self, WinitEvent},
    },
    delegate_compositor, delegate_data_device, delegate_output,
    delegate_seat, delegate_shm, delegate_xdg_shell,
    desktop::{PopupManager, Space, Window, WindowSurfaceType},
    input::{
        keyboard::{keysyms, FilterResult, KeyboardHandle, XkbConfig},
        pointer::{ButtonEvent, CursorImageStatus, MotionEvent, PointerHandle},
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
    utils::{Logical, Point, Scale, Size, Transform, SERIAL_COUNTER},
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

use crate::config::{Action, NoxConfig};
use crate::input::match_keybind;
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
    fn new(display: &Display<Self>, config: NoxConfig, loop_signal: LoopSignal) -> Self {
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

    fn screen_size(&self) -> Size<i32, Logical> {
        self.output.as_ref()
            .and_then(|o| o.current_mode().map(|m| m.size.to_logical(1)))
            .unwrap_or_else(|| Size::from((1920, 1080)))
    }

    fn apply_layout(&mut self) {
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

    fn find_window(&self, id: WindowId) -> Option<Window> {
        self.window_ids.iter()
            .find_map(|(w, &wid)| if wid == id { Some(w.clone()) } else { None })
    }

    fn focused_window(&self) -> Option<Window> {
        let id = self.workspaces[self.active_ws].layout.focused_window()?;
        self.find_window(id)
    }

    // Set keyboard focus — avoids the borrow conflict by cloning the surface first
    fn focus_surface(&mut self, surface: Option<WlSurface>) {
        let serial = SERIAL_COUNTER.next_serial();
        // We need to temporarily take keyboard out of self to avoid borrow conflict
        let kb = self.keyboard.clone();
        kb.set_focus(self, surface, serial);
    }

    fn focus_window(&mut self, win: Option<Window>) {
        let surface = win.as_ref()
            .and_then(|w| w.toplevel())
            .map(|tl| tl.wl_surface().clone());
        if let Some(w) = &win {
            self.space.raise_element(w, true);
        }
        self.focus_surface(surface);
    }

    fn switch_workspace(&mut self, idx: usize) {
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

    fn click_focus_at(&mut self, pos: Point<f64, Logical>) {
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

// ── run() ─────────────────────────────────────────────────────────────────────

pub fn run(config: NoxConfig) {
    let mut event_loop: EventLoop<NoxState> = EventLoop::try_new().unwrap();
    let mut display: Display<NoxState>      = Display::new().unwrap();
    let loop_signal = event_loop.get_signal();

    let mut state = NoxState::new(&display, config, loop_signal);

    // Winit backend
    let (mut winit_backend, mut winit_evt_loop) =
        winit::init::<GlesRenderer>()
            .expect("failed to init winit — run from inside a Wayland/X11 session");

    let mode = Mode { size: winit_backend.window_size(), refresh: 60_000 };

    let output = Output::new("winit-0".into(), PhysicalProperties {
        size: (0, 0).into(), subpixel: Subpixel::Unknown,
        make: "noxwm".into(), model: "winit".into(),
    });
    let _global = output.create_global::<NoxState>(&display.handle());
    output.change_current_state(Some(mode), Some(Transform::Flipped180), None, Some((0,0).into()));
    output.set_preferred(mode);
    state.space.map_output(&output, (0, 0));
    state.output = Some(output.clone());

    let screen: Size<i32, Logical> = mode.size.to_logical(1);
    for ws in &mut state.workspaces { ws.layout.update_screen(screen); }

    // Open Wayland socket
    let listening_socket = ListeningSocketSource::new_auto().unwrap();
    let socket_name = listening_socket.socket_name().to_owned();
    std::env::set_var("WAYLAND_DISPLAY", socket_name.to_string_lossy().as_ref());
    tracing::info!("WAYLAND_DISPLAY={}", socket_name.to_string_lossy());

    // Insert socket into event loop
    let mut dh = display.handle();
    event_loop.handle().insert_source(listening_socket, move |client_stream, _, _state| {
        dh.insert_client(client_stream, std::sync::Arc::new(ClientState::default())).unwrap();
    }).unwrap();

    // Autostart
    for cmd in state.config.autostart.clone() {
        let _ = Command::new("sh").args(["-c", &cmd]).spawn();
    }

    let _damage_tracker = OutputDamageTracker::from_output(&output);

    // Store display handle for use in the socket callback
    // (we'll use a separate approach — process clients inline)
    
    loop {
        winit_evt_loop.dispatch_new_events(|event| {
            match event {
                WinitEvent::Resized { size, .. } => {
                    let s: Size<i32, Logical> = size.to_logical(1);
                    for ws in &mut state.workspaces { ws.layout.update_screen(s); }
                    state.apply_layout();
                }

                WinitEvent::Input(input) => match input {
                    InputEvent::Keyboard { event } => {
                        let serial    = SERIAL_COUNTER.next_serial();
                        let time      = Event::time_msec(&event);
                        let keycode   = event.key_code();
                        let key_state = event.state();
                        let kb = state.keyboard.clone();
                        kb.input::<(), _>(
                            &mut state, keycode, key_state, serial, time,
                            |s, mods, handle| {
                                if key_state != KeyState::Pressed { return FilterResult::Forward; }
                                let sym      = handle.modified_sym();
                                let key_name = keysym_name(sym.raw());
                                let binds    = s.config.keybinds.clone();
                                if let Some(action) = match_keybind(&binds, &key_name, mods) {
                                    match action.clone() {
                                        Action::Exec(cmd) => { let _ = Command::new("sh").args(["-c", &cmd]).spawn(); }
                                        Action::Close => {
                                            if let Some(w) = s.focused_window() {
                                                if let Some(tl) = w.toplevel() { tl.send_close(); }
                                            }
                                        }
                                        Action::FocusLeft  => {
                                            s.workspaces[s.active_ws].layout.focus_prev();
                                            let f = s.focused_window(); s.focus_window(f);
                                        }
                                        Action::FocusRight => {
                                            s.workspaces[s.active_ws].layout.focus_next();
                                            let f = s.focused_window(); s.focus_window(f);
                                        }
                                        Action::Workspace(idx) => s.switch_workspace(idx),
                                        Action::Quit          => s.should_quit = true,
                                        Action::Fullscreen    => {}
                                    }
                                    return FilterResult::Intercept(());
                                }
                                FilterResult::Forward
                            },
                        );
                    }

                    InputEvent::PointerMotion { event } => {
                        let pos    = state.pointer.current_location() + PointerMotionEvent::<WinitInput>::delta(&event);
                        let serial = SERIAL_COUNTER.next_serial();
                        let under  = surface_under(&state.space, pos);
                        let ptr = state.pointer.clone();
                        ptr.motion(&mut state, under,
                            &MotionEvent { location: pos, serial, time: Event::<WinitInput>::time_msec(&event) });
                        ptr.frame(&mut state);
                    }

                    InputEvent::PointerButton { event } => {
                        let serial = SERIAL_COUNTER.next_serial();
                        let pos    = state.pointer.current_location();
                        state.click_focus_at(pos);
                        let _under = surface_under(&state.space, pos);
                        let ptr = state.pointer.clone();
                        ptr.button(&mut state, &ButtonEvent {
                            serial, time: Event::<WinitInput>::time_msec(&event),
                            button: event.button_code(), state: event.state(),
                        });
                        ptr.frame(&mut state);
                    }

                    _ => {}
                },

                WinitEvent::CloseRequested => state.should_quit = true,
                _ => {}
            }
        });

        // Render
        let render_elements: Vec<WaylandSurfaceRenderElement<GlesRenderer>> =
            state.space.elements()
                .filter(|w| w.alive())
                .flat_map(|window| {
                    let loc = state.space.element_location(window)
                        .unwrap_or_default()
                        .to_physical_precise_round(1);
                    render_elements_from_surface_tree(
                        winit_backend.renderer(),
                        window.toplevel().unwrap().wl_surface(),
                        loc,
                        Scale::from(1.0),
                        1.0,
                        smithay::backend::renderer::element::Kind::Unspecified,
                    )
                })
                .collect();

        {
            use smithay::backend::renderer::{Renderer, Frame};
            use smithay::backend::renderer::element::RenderElement;
            let size = mode.size;
            let (renderer, mut target) = winit_backend.bind().unwrap();
            let mut frame = renderer
                .render(&mut target, size, Transform::Flipped180)
                .unwrap();
            frame.clear(
                [0.08_f32, 0.08, 0.08, 1.0].into(),
                &[smithay::utils::Rectangle::new((0,0).into(), size)],
            ).unwrap();
            for element in &render_elements {
                let src  = element.src();
                let dst  = element.geometry(Scale::from(1.0));
                element.draw(&mut frame, src, dst, &[], &[smithay::utils::Rectangle::new((0,0).into(), size)]).unwrap();
            }
            frame.finish().unwrap();
        }

        winit_backend.submit(None).unwrap();

        display.flush_clients().unwrap();
        event_loop.dispatch(Some(Duration::from_millis(1)), &mut state).unwrap();

        if state.should_quit { break; }
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

fn surface_under(
    space: &Space<Window>,
    pos:   Point<f64, Logical>,
) -> Option<(WlSurface, Point<f64, Logical>)> {
    space.element_under(pos).and_then(|(win, win_loc)| {
        let rel = pos - win_loc.to_f64();
        win.surface_under(rel, WindowSurfaceType::ALL)
            .map(|(surface, offset)| (surface, win_loc.to_f64() + offset.to_f64()))
    })
}

fn keysym_name(sym: u32) -> String {
    match sym {
        keysyms::KEY_Return | keysyms::KEY_KP_Enter => "Return".into(),
        keysyms::KEY_space     => "space".into(),
        keysyms::KEY_Tab       => "Tab".into(),
        keysyms::KEY_Escape    => "Escape".into(),
        keysyms::KEY_BackSpace => "BackSpace".into(),
        keysyms::KEY_Delete    => "Delete".into(),
        keysyms::KEY_Left      => "Left".into(),
        keysyms::KEY_Right     => "Right".into(),
        keysyms::KEY_Up        => "Up".into(),
        keysyms::KEY_Down      => "Down".into(),
        s if s >= keysyms::KEY_F1 && s <= keysyms::KEY_F12 => format!("F{}", s - keysyms::KEY_F1 + 1),
        s if s >= keysyms::KEY_a && s <= keysyms::KEY_z =>
            char::from_u32(s).map(|c| c.to_string()).unwrap_or_default(),
        s if s >= keysyms::KEY_A && s <= keysyms::KEY_Z =>
            char::from_u32(s).map(|c| c.to_ascii_lowercase().to_string()).unwrap_or_default(),
        s if s >= keysyms::KEY_0 && s <= keysyms::KEY_9 =>
            char::from_u32(s).map(|c| c.to_string()).unwrap_or_default(),
        _ => format!("{sym:#x}"),
    }
}
