//! claudid — the claudi background daemon.

use anyhow::Result;
use tracing_subscriber::EnvFilter;

mod state;
mod render;
mod link;
mod hooks;
mod ipc;
mod tailer;
mod cost;
mod font;

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")))
        .init();
    tracing::info!("claudid starting");
    daemon_main().await
}

async fn daemon_main() -> Result<()> {
    let state = state::DaemonState::new();
    let shared = std::sync::Arc::new(tokio::sync::Mutex::new(state));

    // IPC: Unix socket for hook CLI + claudi CLI commands
    let ipc_handle = tokio::spawn(ipc::serve(shared.clone()));

    // Transcript tailer: watch ~/.claude/projects for token/cost data
    let tailer_handle = tokio::spawn(tailer::run(shared.clone()));

    // Device link: connect to /dev/cu.usbmodem*, render frames, receive touch
    let link_handle = tokio::spawn(link::run(shared.clone()));

    tokio::select! {
        r = ipc_handle => { tracing::error!("ipc exited: {:?}", r); }
        r = tailer_handle => { tracing::error!("tailer exited: {:?}", r); }
        r = link_handle => { tracing::error!("link exited: {:?}", r); }
        _ = tokio::signal::ctrl_c() => { tracing::info!("ctrl-c, exiting"); }
    }

    Ok(())
}
