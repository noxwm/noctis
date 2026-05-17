mod config;
mod core;
mod input;
mod layout;

use tracing::info;
use tracing_subscriber::EnvFilter;

fn main() {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .init();

    let config = config::NoxConfig::load();
    info!("config loaded — gaps={} border_width={}", config.gaps, config.border_width);

    // Auto-detect: if DISPLAY or WAYLAND_DISPLAY is set, use winit (nested).
    // Otherwise use udev backend (real TTY).
    let is_nested = std::env::var("WAYLAND_DISPLAY").is_ok()
        || std::env::var("DISPLAY").is_ok();

    if is_nested {
        info!("running nested (winit backend)");
        core::winit::run(config);
    } else {
        info!("running on TTY (udev/DRM backend)");
        core::udev::run(config);
    }
}
