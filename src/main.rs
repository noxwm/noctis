mod config;
mod core;
mod input;
mod layout;

use tracing::info;
use tracing_subscriber::EnvFilter;

fn main() {
    // Init logging — set RUST_LOG=debug for verbose output
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .init();

    info!("noxwm starting");

    let config = config::NoxConfig::load();
    info!("config loaded — gaps={} border_width={}", config.gaps, config.border_width);

    core::run(config);
}
