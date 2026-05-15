//! `claudi` — the CLI front-end to the daemon.

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::time::Duration;

#[derive(Parser)]
#[command(name = "claudi", about = "claudi controller — talks to the running daemon")]
struct Args {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Show daemon status (sessions, device connection).
    Status,
    /// Install Claude Code hooks into ~/.claude/settings.json.
    InstallHooks {
        /// Don't write — just print what would be merged.
        #[arg(long)]
        dry_run: bool,
    },
    /// Internal: forward a hook event to the daemon. Reads JSON from stdin.
    Hook {
        /// Hook event name (session_start, pre_tool_use, post_tool_use, ...).
        event: String,
    },
    /// Reboot the device. By default into BOOTSEL so a UF2 can be flashed.
    Reboot {
        /// Reboot into BOOTSEL (default) vs soft application reboot.
        #[arg(long, default_value_t = false)]
        app: bool,
    },
    /// Build the firmware and flash it to the device (assumes device is running claudi firmware).
    Flash {
        /// Path to the UF2. Defaults to firmware/build/claudi_firmware.uf2
        #[arg(default_value = "firmware/build/claudi_firmware.uf2")]
        uf2: String,
    },
    /// Render the current device frame to a PNG file (debug).
    RenderPng {
        /// Output PNG path
        #[arg(default_value = "/tmp/claudi-frame.png")]
        out: String,
        /// Force a particular screen first (sessions, feed, metrics, approval)
        #[arg(long)]
        screen: Option<String>,
        /// Focus a session id before rendering (useful for `--screen feed`)
        #[arg(long)]
        focus: Option<String>,
    },
    /// Send a synthetic event for testing (no Claude Code required).
    Simulate {
        /// Event to simulate.
        #[arg(long, default_value = "pre_tool_use")]
        event: String,
        /// Session id (uuid). Defaults to a stable demo id.
        #[arg(long, default_value = "demo-session-0001")]
        session: String,
        /// Tool name to simulate (Bash, Edit, ...)
        #[arg(long, default_value = "Bash")]
        tool: String,
        /// Tool input text (e.g. command for Bash, file_path for Edit)
        #[arg(long, default_value = "echo hello")]
        input: String,
    },
}

fn main() -> Result<()> {
    let args = Args::parse();
    match args.cmd {
        Cmd::Status => status(),
        Cmd::InstallHooks { dry_run } => install_hooks(dry_run),
        Cmd::Hook { event } => hook(&event),
        Cmd::Simulate { event, session, tool, input } => simulate(event, session, tool, input),
        Cmd::RenderPng { out, screen, focus } => render_png(out, screen, focus),
        Cmd::Reboot { app } => reboot(if app { 0 } else { 1 }),
        Cmd::Flash { uf2 } => flash(uf2),
    }
}

#[derive(Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
enum Request {
    Status,
    Hook {
        event: String,
        session_id: Option<String>,
        cwd: Option<String>,
        transcript_path: Option<String>,
        payload: Value,
    },
    RenderPng,
    SetScreen { name: String },
    Focus { session_id: String },
    Reboot { mode: u8 },
}

#[derive(Deserialize, Debug)]
#[serde(tag = "kind", rename_all = "snake_case")]
enum Response {
    Ok,
    Status {
        sessions: usize,
        device_connected: bool,
        backlight: u8,
    },
    Approval {
        decision: String,
    },
    Png {
        base64: String,
    },
    Error {
        message: String,
    },
}

fn socket_path() -> PathBuf {
    PathBuf::from("/tmp/claudi.sock")
}

fn call(req: &Request) -> Result<Response> {
    let mut stream = UnixStream::connect(socket_path())
        .with_context(|| format!("connect to {}", socket_path().display()))?;
    stream.set_read_timeout(Some(Duration::from_secs(60)))?;
    let body = serde_json::to_vec(req)?;
    stream.write_all(&(body.len() as u32).to_le_bytes())?;
    stream.write_all(&body)?;
    let mut len_buf = [0u8; 4];
    stream.read_exact(&mut len_buf)?;
    let len = u32::from_le_bytes(len_buf) as usize;
    let mut payload = vec![0u8; len];
    stream.read_exact(&mut payload)?;
    Ok(serde_json::from_slice(&payload)?)
}

fn try_call(req: &Request) -> Option<Response> {
    call(req).ok()
}

fn status() -> Result<()> {
    match call(&Request::Status)? {
        Response::Status { sessions, device_connected, backlight } => {
            println!("daemon: connected");
            println!("sessions: {sessions}");
            println!("device:   {}", if device_connected { "connected" } else { "disconnected" });
            println!("backlight: {backlight}");
        }
        other => println!("unexpected response: {other:?}"),
    }
    Ok(())
}

fn hook(event: &str) -> Result<()> {
    let mut s = String::new();
    std::io::stdin().read_to_string(&mut s).ok();
    let payload: Value = if s.trim().is_empty() {
        Value::Null
    } else {
        serde_json::from_str(&s).unwrap_or(Value::Null)
    };

    let session_id = payload.get("session_id").and_then(|v| v.as_str()).map(|s| s.to_string());
    let cwd = payload.get("cwd").and_then(|v| v.as_str()).map(|s| s.to_string());
    let transcript_path = payload.get("transcript_path").and_then(|v| v.as_str()).map(|s| s.to_string());

    let req = Request::Hook {
        event: event.to_string(),
        session_id,
        cwd,
        transcript_path,
        payload,
    };
    // Best-effort: if the daemon isn't running, exit 0 silently so we never
    // block Claude Code.
    let _ = try_call(&req);
    Ok(())
}

fn simulate(event: String, session: String, tool: String, input: String) -> Result<()> {
    let cwd = std::env::current_dir().ok().map(|p| p.display().to_string());
    let mut payload = serde_json::json!({
        "session_id": session,
        "cwd": cwd,
    });
    if event.contains("tool") {
        payload["tool_name"] = serde_json::Value::String(tool.clone());
        let key = match tool.as_str() {
            "Bash" => "command",
            "Read" | "Edit" | "Write" => "file_path",
            "Grep" | "Glob" => "pattern",
            "WebFetch" => "url",
            _ => "command",
        };
        payload["tool_input"] = serde_json::json!({ key: input });
    } else if event.contains("prompt") {
        payload["prompt"] = serde_json::Value::String(input.clone());
    } else if event.contains("notification") {
        payload["message"] = serde_json::Value::String(input.clone());
    }
    let req = Request::Hook {
        event,
        session_id: Some(session),
        cwd,
        transcript_path: None,
        payload,
    };
    match call(&req)? {
        Response::Ok => println!("ok"),
        other => println!("{other:?}"),
    }
    Ok(())
}

fn reboot(mode: u8) -> Result<()> {
    match call(&Request::Reboot { mode })? {
        Response::Ok => {
            println!("device asked to reboot ({})", if mode == 1 { "BOOTSEL" } else { "app" });
            Ok(())
        }
        other => anyhow::bail!("unexpected response: {other:?}"),
    }
}

fn flash(uf2: String) -> Result<()> {
    use std::process::Command;
    if !std::path::Path::new(&uf2).exists() {
        anyhow::bail!("UF2 not found at {uf2} (try `make firmware` first)");
    }
    println!("→ reboot to BOOTSEL via daemon");
    let _ = reboot(1);
    // Give the device a moment to enumerate as picoboot
    std::thread::sleep(Duration::from_secs(2));
    println!("→ flashing {uf2}");
    let status = Command::new("picotool")
        .args(["load", &uf2, "-f"])
        .status()?;
    if !status.success() {
        anyhow::bail!("picotool load failed");
    }
    println!("→ rebooting to application");
    let _ = Command::new("picotool").args(["reboot"]).status();
    Ok(())
}

fn render_png(out: String, screen: Option<String>, focus: Option<String>) -> Result<()> {
    use base64::Engine;
    if let Some(sid) = focus {
        let _ = call(&Request::Focus { session_id: sid })?;
    }
    if let Some(s) = screen {
        let _ = call(&Request::SetScreen { name: s })?;
        std::thread::sleep(Duration::from_millis(150));
    }
    match call(&Request::RenderPng)? {
        Response::Png { base64 } => {
            let bytes = base64::engine::general_purpose::STANDARD.decode(base64)?;
            std::fs::write(&out, &bytes)?;
            println!("wrote {} ({} bytes)", out, bytes.len());
            Ok(())
        }
        other => anyhow::bail!("unexpected response: {other:?}"),
    }
}

fn install_hooks(dry_run: bool) -> Result<()> {
    let path = dirs::home_dir()
        .context("no home dir")?
        .join(".claude")
        .join("settings.json");
    if !path.parent().map(|p| p.exists()).unwrap_or(false) {
        std::fs::create_dir_all(path.parent().unwrap())?;
    }
    let existing: Value = if path.exists() {
        let s = std::fs::read_to_string(&path)?;
        serde_json::from_str(&s).unwrap_or(Value::Object(Default::default()))
    } else {
        Value::Object(Default::default())
    };

    let mut next = existing.clone();
    let hooks = next.as_object_mut().unwrap()
        .entry("hooks".to_string())
        .or_insert_with(|| Value::Object(Default::default()));

    let events = [
        ("SessionStart", "session_start"),
        ("UserPromptSubmit", "user_prompt_submit"),
        ("PreToolUse", "pre_tool_use"),
        ("PostToolUse", "post_tool_use"),
        ("Notification", "notification"),
        ("Stop", "stop"),
        ("SessionEnd", "session_end"),
    ];

    let map = hooks.as_object_mut().unwrap();
    for (event, kind) in events {
        let entry = serde_json::json!({
            "hooks": [
                { "type": "command", "command": format!("claudi hook {kind}") }
            ]
        });
        // Replace any existing claudi entry; preserve user-added hooks for the same event.
        let list = map.entry(event.to_string()).or_insert_with(|| Value::Array(vec![]));
        if let Some(arr) = list.as_array_mut() {
            arr.retain(|item| {
                !item.to_string().contains("\"claudi hook ")
            });
            arr.push(entry);
        }
    }

    if dry_run {
        println!("{}", serde_json::to_string_pretty(&next)?);
        return Ok(());
    }
    let body = serde_json::to_string_pretty(&next)?;
    std::fs::write(&path, body)?;
    println!("wrote hooks to {}", path.display());
    Ok(())
}
