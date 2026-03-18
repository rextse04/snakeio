use std::{
    fs,
    net::UdpSocket,
    path::PathBuf,
    sync::Arc,
    thread,
};

use rand::Rng;
use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, State};
use tauri::ipc::{InvokeBody, Request};

#[derive(Debug, Clone, Serialize, Deserialize)]
struct AppConfig {
    server_addr: String,

    debug_drop_outgoing_enabled: bool,
    /// 0.0 = never drop, 1.0 = always drop
    debug_drop_outgoing_chance: f64,

    debug_drop_incoming_enabled: bool,
    /// 0.0 = never drop, 1.0 = always drop
    debug_drop_incoming_chance: f64,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            server_addr: "localhost:50003".to_string(),
            debug_drop_outgoing_enabled: false,
            debug_drop_outgoing_chance: 0.0,
            debug_drop_incoming_enabled: false,
            debug_drop_incoming_chance: 0.0,
        }
    }
}

struct UdpState {
    socket: Arc<UdpSocket>,
    config: Arc<AppConfig>,
}

fn load_config(app: &AppHandle) -> Result<AppConfig, String> {
    let app_config_dir = app
        .path()
        .app_config_dir()
        .map_err(|e| format!("Failed to get app config dir: {e}"))?;

    let config_path: PathBuf = app_config_dir.join("config.json");

    if let Some(parent) = config_path.parent() {
        fs::create_dir_all(parent)
            .map_err(|e| format!("Failed to create config dir: {e}"))?;
    }

    if !config_path.exists() {
        let default_config = AppConfig::default();
        let json = serde_json::to_string_pretty(&default_config)
            .map_err(|e| format!("Failed to serialize default config: {e}"))?;

        fs::write(&config_path, json)
            .map_err(|e| format!("Failed to write default config: {e}"))?;

        println!("Created default config at: {}", config_path.display());
        return Ok(default_config);
    }

    let contents = fs::read_to_string(&config_path)
        .map_err(|e| format!("Failed to read config file: {e}"))?;

    let mut config: AppConfig = serde_json::from_str(&contents)
        .map_err(|e| format!("Failed to parse config file: {e}"))?;

    // Defensive clamping
    config.debug_drop_outgoing_chance = config.debug_drop_outgoing_chance.clamp(0.0, 1.0);
    config.debug_drop_incoming_chance = config.debug_drop_incoming_chance.clamp(0.0, 1.0);

    println!("Loaded config from: {}", config_path.display());

    Ok(config)
}

fn should_drop_packet(enabled: bool, chance: f64) -> bool {
    if !enabled {
        return false;
    }

    let chance = chance.clamp(0.0, 1.0);
    if chance <= 0.0 {
        return false;
    }

    let mut rng = rand::thread_rng();
    rng.gen_bool(chance)
}

#[tauri::command]
fn send_packet(state: State<UdpState>, request: Request<'_>) -> Result<(), String> {
    let InvokeBody::Raw(data) = request.body() else {
        return Err("Invalid request body".into());
    };

    // Simulate outgoing packet loss
    if should_drop_packet(
        state.config.debug_drop_outgoing_enabled,
        state.config.debug_drop_outgoing_chance,
    ) {
        println!("[DEBUG] Dropped outgoing packet ({} bytes)", data.len());
        return Ok(());
    }

    state
        .socket
        .send(data)
        .map_err(|e| e.to_string())?;

    Ok(())
}

#[tauri::command]
fn exit_app(app: AppHandle) {
    app.exit(0);
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .setup(move |app| {
            let config = load_config(app.handle())?;
            println!("Loaded config: {:?}", config);

            let socket = UdpSocket::bind("0.0.0.0:0")
                .map_err(|e| format!("Failed to bind UDP socket: {e}"))?;

            socket
                .connect(&config.server_addr)
                .map_err(|e| format!("Failed to connect to server {}: {e}", config.server_addr))?;

            let socket = Arc::new(socket);
            let config = Arc::new(config);

            let window = app
                .get_webview_window("main")
                .ok_or("Failed to get main window")?;

            let recv_socket = socket.clone();
            let recv_config = config.clone();

            thread::spawn(move || {
                let mut buf = [0u8; 1024 + 32];

                loop {
                    match recv_socket.recv(&mut buf) {
                        Ok(len) => {
                            // Simulate incoming packet loss
                            if should_drop_packet(
                                recv_config.debug_drop_incoming_enabled,
                                recv_config.debug_drop_incoming_chance,
                            ) {
                                println!("[DEBUG] Dropped incoming packet ({} bytes)", len);
                                continue;
                            }

                            let data = buf[..len].to_vec();

                            if let Err(err) = window.emit("recv_packet", data) {
                                eprintln!("Failed to emit recv_packet: {err}");
                                break;
                            }
                        }
                        Err(err) => {
                            eprintln!("UDP receive error: {err}");
                            break;
                        }
                    }
                }
            });

            app.manage(UdpState { socket, config });

            Ok(())
        })
        .invoke_handler(tauri::generate_handler![send_packet, exit_app])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}