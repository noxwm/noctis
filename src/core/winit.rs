use crate::config::{NoxConfig, Action};
use crate::core::state::*;
use crate::input::match_keybind;
use smithay::{{
    backend::{{
        input::{{Event, InputEvent, KeyState, KeyboardKeyEvent, PointerButtonEvent, PointerMotionEvent}},
        renderer::{{
            damage::OutputDamageTracker,
            element::surface::{{render_elements_from_surface_tree, WaylandSurfaceRenderElement}},
            gles::GlesRenderer,
        }},
        winit::{{self, WinitEvent, WinitInput}},
    }},
    utils::{{Scale, Size, Transform, SERIAL_COUNTER, Logical, Point}},
    output::{{Mode, Output, PhysicalProperties, Subpixel}},
    reexports::{{
        calloop::EventLoop,
        wayland_server::{{Display, protocol::wl_surface::WlSurface}},
    }},
    wayland::socket::ListeningSocketSource,
    desktop::{Space, Window, WindowSurfaceType},
}};
use smithay::backend::renderer::element::Element;
use smithay::utils::IsAlive;
use smithay::input::pointer::{{ButtonEvent, MotionEvent}};
use smithay::input::keyboard::{{FilterResult, keysyms}};
use std::process::Command;
use std::time::Duration;

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

pub fn surface_under(
    space: &Space<Window>,
    pos:   Point<f64, Logical>,
) -> Option<(WlSurface, Point<f64, Logical>)> {
    space.element_under(pos).and_then(|(win, win_loc)| {
        let rel = pos - win_loc.to_f64();
        win.surface_under(rel, WindowSurfaceType::ALL)
            .map(|(surface, offset)| (surface, win_loc.to_f64() + offset.to_f64()))
    })
}

pub fn keysym_name(sym: u32) -> String {
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
