/// udev/DRM backend — runs on a real TTY without an existing compositor.
///
/// Uses libseat for session management (works with seatd or logind).
/// Opens the DRM device via udev, renders with GLES via GBM.

use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use smithay::{
    backend::{
        allocator::gbm::{GbmAllocator, GbmBufferFlags, GbmDevice},
        drm::{
            compositor::DrmCompositor, DrmDevice, DrmDeviceFd, DrmEvent,
            DrmNode, NodeType,
        },
        egl::EGLDisplay,
        input::InputEvent,
        libinput::{LibinputInputBackend, LibinputSessionInterface},
        renderer::gles::GlesRenderer,
        session::{libseat::LibSeatSession, Session},
        udev::UdevBackend,
    },
    reexports::{
        calloop::EventLoop,
        drm::control::{connector, Device, ModeTypeFlags},
        input::Libinput,
        wayland_server::Display,
    },
    utils::{DeviceFd, IsAlive},
    wayland::socket::ListeningSocketSource,
};

use crate::config::NoxConfig;
use crate::core::state::{ClientState, NoxState};

pub fn run(config: NoxConfig) {
    let mut event_loop: EventLoop<NoxState> = EventLoop::try_new().unwrap();
    let mut display: Display<NoxState>      = Display::new().unwrap();
    let loop_signal = event_loop.get_signal();

    // ── Session via libseat ───────────────────────────────────────────────────
    let (session, notifier) = LibSeatSession::new()
        .expect("failed to create libseat session — is seatd running?");

    tracing::info!("session opened: seat={}", session.seat());

    let mut state = NoxState::new(&display, config, loop_signal);

    // ── Wayland socket ────────────────────────────────────────────────────────
    let listening_socket = ListeningSocketSource::new_auto().unwrap();
    let socket_name = listening_socket.socket_name().to_owned();
    std::env::set_var("WAYLAND_DISPLAY", socket_name.to_string_lossy().as_ref());
    tracing::info!("WAYLAND_DISPLAY={}", socket_name.to_string_lossy());

    let dh = display.handle();
    event_loop.handle().insert_source(listening_socket, move |stream, _, _state| {
        dh.insert_client(stream, Arc::new(ClientState::default())).unwrap();
    }).unwrap();

    // ── libinput ──────────────────────────────────────────────────────────────
    let mut libinput_context = Libinput::new_with_udev(
        LibinputSessionInterface::from(session.clone()),
    );
    libinput_context.udev_assign_seat(&session.seat()).unwrap();

    let libinput_backend = LibinputInputBackend::new(libinput_context.clone());

    event_loop.handle().insert_source(libinput_backend, |event, _, state| {
        handle_input(event, state);
    }).unwrap();

    // ── udev DRM device discovery ─────────────────────────────────────────────
    let udev_backend = UdevBackend::new(&session.seat())
        .expect("failed to create udev backend");

    // Find primary GPU
    let (drm_node, _fd) = udev_backend
        .device_list()
        .find_map(|(dev_id, path)| {
            DrmNode::from_path(path)
                .ok()
                .filter(|node| node.ty() == NodeType::Primary)
                .map(|node| (node, dev_id))
        })
        .expect("no primary DRM device found");

    tracing::info!("using DRM device: {:?}", drm_node);

    // Open DRM device
    let fd = session.open(
        drm_node.dev_path().unwrap().as_path(),
        smithay::reexports::rustix::fs::OFlags::RDWR | smithay::reexports::rustix::fs::OFlags::CLOEXEC | smithay::reexports::rustix::fs::OFlags::NOCTTY | smithay::reexports::rustix::fs::OFlags::NONBLOCK,
    ).expect("failed to open DRM device");

    let drm_fd  = DrmDeviceFd::new(DeviceFd::from(fd));
    let (drm, drm_notifier) = DrmDevice::new(drm_fd.clone(), true)
        .expect("failed to create DRM device");

    // GBM + GLES renderer
    let gbm = GbmDevice::new(drm_fd).expect("failed to create GBM device");
    let egl = EGLDisplay::new(gbm.clone()).expect("failed to create EGL display");
    let ctx = smithay::backend::egl::EGLContext::new(&egl).expect("failed to create EGL context");
    let mut renderer = unsafe {
        GlesRenderer::new(ctx).expect("failed to create GLES renderer")
    };

    // Find a connector + CRTC + mode
    let drm_res = drm.resource_handles().expect("failed to get DRM resources");

    let connector = drm_res
        .connectors()
        .iter()
        .find_map(|&conn| {
            let info = drm.get_connector(conn, false).ok()?;
            if info.state() == connector::State::Connected {
                Some(info)
            } else {
                None
            }
        })
        .expect("no connected display found");

    let mode = connector
        .modes()
        .iter()
        .find(|m| m.mode_type().contains(ModeTypeFlags::PREFERRED))
        .or_else(|| connector.modes().first())
        .copied()
        .expect("no display mode found");

    tracing::info!(
        "display: {}x{}@{}",
        mode.size().0, mode.size().1,
        mode.vrefresh()
    );

    // Find compatible CRTC for this connector
    let crtc = drm_res
        .crtcs()
        .into_iter()
        .find(|&crtc_id| {
            connector
                .encoders()
                .iter()
                .any(|encoder_id| {
                    if let Ok(encoder_info) = drm.get_encoder(*encoder_id) {
                        encoder_info.possible_crtcs().contains(&crtc_id)
                    } else {
                        false
                    }
                })
        })
        .expect("no compatible CRTC found for connector");

    // Create DRM surface using create_surface method
    let surface = drm.create_surface(
        crtc,
        mode,
        &[connector.handle()],
    ).expect("failed to create DRM surface");

    // Create GBM allocator for buffer management
    let gbm_allocator = GbmAllocator::new(
        gbm.clone(),
        GbmBufferFlags::RENDERING | GbmBufferFlags::SCANOUT,
    );

    // Create GBM framebuffer exporter
    use smithay::backend::drm::exporter::gbm::GbmFramebufferExporter;
    use drm::node::DrmNode;
    
    let drm_node = DrmNode::from_file(&drm).ok();
    let fbuf_exporter = Arc::new(Mutex::new(
        GbmFramebufferExporter::new(gbm.clone(), drm_node)
    ));

    // Color formats the primary plane supports (using Smithay's Fourcc)
    let color_formats = vec![
        smithay::backend::allocator::Fourcc::Argb8888,
        smithay::backend::allocator::Fourcc::Xrgb8888,
    ];

    // Renderer formats (Smithay's DrmFormat with Modifier)
    let renderer_formats = vec![
        smithay::backend::drm::DrmFormat {
            code: smithay::backend::allocator::Fourcc::Argb8888,
            modifier: smithay::backend::allocator::Modifier::Linear,
        },
        smithay::backend::drm::DrmFormat {
            code: smithay::backend::allocator::Fourcc::Xrgb8888,
            modifier: smithay::backend::allocator::Modifier::Linear,
        },
    ];

    // Build DRM compositor with correct 9 arguments
    let output = smithay::output::Output::new("drm-0".into(), smithay::output::PhysicalProperties {
        size: (0, 0).into(),
        subpixel: smithay::output::Subpixel::Unknown,
        make: "noxwm".into(),
        model: "drm".into(),
    });

    let mut drm_compositor = DrmCompositor::new(
        &output,                           // 1. output_mode_source
        surface,                           // 2. surface
        None,                              // 3. planes (None = use all)
        gbm_allocator,                     // 4. allocator
        fbuf_exporter,                     // 5. framebuffer_exporter (wrapped in Arc<Mutex<>>)
        color_formats.into_iter(),         // 6. color_formats
        renderer_formats.into_iter(),      // 7. renderer_formats  
        smithay::utils::Size::from((64u32, 64u32)),  // 8. cursor_size
        Some(gbm.clone()),                 // 9. gbm device for cursor plane
    ).expect("failed to create DRM compositor");

    // Register DRM events (page flip notifications)
    event_loop.handle().insert_source(drm_notifier, move |event, _, _state| {
        if let DrmEvent::VBlank(_crtc) = event {
            // Page flip complete — we can render the next frame
        }
    }).unwrap();

    // Update workspace screen sizes
    let screen_size: smithay::utils::Size<i32, smithay::utils::Physical> = smithay::utils::Size::from((mode.size().0 as i32, mode.size().1 as i32));
    for ws in &mut state.workspaces {
        ws.layout.update_screen(screen_size.to_logical(1));
    }

    // Autostart
    for cmd in state.config.autostart.clone() {
        let _ = Command::new("sh").args(["-c", &cmd]).spawn();
    }

    // ── Main loop ─────────────────────────────────────────────────────────────
    loop {
        // Render current frame
        let render_elements: Vec<_> = state.space.elements()
            .filter(|w| w.alive())
            .flat_map(|window| {
                let loc = state.space.element_location(window)
                    .unwrap_or_default()
                    .to_physical_precise_round(1);
                smithay::backend::renderer::element::surface::render_elements_from_surface_tree(
                    &mut renderer,
                    window.toplevel().unwrap().wl_surface(),
                    loc,
                    smithay::utils::Scale::from(1.0),
                    1.0,
                    smithay::backend::renderer::element::Kind::Unspecified,
                )
            })
            .collect();

        drm_compositor
            .render_frame::<_, smithay::backend::renderer::element::surface::WaylandSurfaceRenderElement<GlesRenderer>>(
                &mut renderer,
                &render_elements,
                [0.08, 0.08, 0.08, 1.0],
                smithay::backend::drm::compositor::FrameFlags::DEFAULT,
            )
            .map_err(|e| tracing::warn!("render error: {e:?}"))
            .ok();

        drm_compositor.queue_frame(None).ok();

        display.flush_clients().unwrap();
        event_loop.dispatch(Some(Duration::from_millis(4)), &mut state).unwrap();

        if state.should_quit { break; }
    }
}

fn handle_input(event: InputEvent<smithay::backend::libinput::LibinputInputBackend>, state: &mut NoxState) {
    use smithay::backend::input::*;
    use smithay::utils::SERIAL_COUNTER;
    use crate::config::Action;
    use crate::input::match_keybind;

    match event {
        InputEvent::Keyboard { event } => {
            let serial    = SERIAL_COUNTER.next_serial();
            let time      = event.time_msec();
            let keycode   = event.key_code();
            let key_state = event.state();
            let kb = state.keyboard.clone();
            kb.input::<(), _>(
                state, keycode, key_state, serial, time,
                |s, mods, handle| {
                    if key_state != KeyState::Pressed { return smithay::input::keyboard::FilterResult::Forward; }
                    let sym      = handle.modified_sym();
                    let key_name = crate::core::winit::keysym_name(sym.raw());
                    let binds    = s.config.keybinds.clone();
                    if let Some(action) = match_keybind(&binds, &key_name, mods) {
                        match action.clone() {
                            Action::Exec(cmd) => { let _ = std::process::Command::new("sh").args(["-c", &cmd]).spawn(); }
                            Action::Close => {
                                if let Some(w) = s.focused_window() {
                                    if let Some(tl) = w.toplevel() { tl.send_close(); }
                                }
                            }
                            Action::FocusLeft  => { s.workspaces[s.active_ws].layout.focus_prev(); let f = s.focused_window(); s.focus_window(f); }
                            Action::FocusRight => { s.workspaces[s.active_ws].layout.focus_next(); let f = s.focused_window(); s.focus_window(f); }
                            Action::Workspace(idx) => s.switch_workspace(idx),
                            Action::Quit       => s.should_quit = true,
                            Action::Fullscreen => {}
                        }
                        return smithay::input::keyboard::FilterResult::Intercept(());
                    }
                    smithay::input::keyboard::FilterResult::Forward
                },
            );
        }

        InputEvent::PointerMotion { event } => {
            let pos    = state.pointer.current_location() + event.delta();
            let serial = SERIAL_COUNTER.next_serial();
            let under  = crate::core::winit::surface_under(&state.space, pos);
            let ptr = state.pointer.clone();
            ptr.motion(state, under, &smithay::input::pointer::MotionEvent {
                location: pos, serial, time: event.time_msec(),
            });
            ptr.frame(state);
        }

        InputEvent::PointerButton { event } => {
            let serial = SERIAL_COUNTER.next_serial();
            let pos    = state.pointer.current_location();
            state.click_focus_at(pos);
            let _under = crate::core::winit::surface_under(&state.space, pos);
            let ptr = state.pointer.clone();
            ptr.button(state, &smithay::input::pointer::ButtonEvent {
                serial, time: event.time_msec(),
                button: event.button_code(), state: event.state(),
            });
            ptr.frame(state);
        }

        _ => {}
    }
}
