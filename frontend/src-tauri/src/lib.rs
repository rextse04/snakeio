use std::{
    net::UdpSocket,
    sync::Arc,
    thread,
};

use tauri::{Manager, State, Emitter, AppHandle};

struct UdpState {
    socket: Arc<UdpSocket>,
}

#[tauri::command]
fn send_packet(state: State<UdpState>, data: Vec<u8>) -> Result<(), String> {
    state
        .socket
        .send(&data)
        .map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
fn exit_app(app: AppHandle) {
    app.exit(0);
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // Change this to your server address
    let server_addr = "127.0.0.1:50004";

    // Bind to any available local port
    let socket = UdpSocket::bind("0.0.0.0:0")
        .expect("Failed to bind UDP socket");

    socket
        .connect(server_addr)
        .expect("Failed to connect to server");

    let socket = Arc::new(socket);

    tauri::Builder::default()
        .setup({
            let socket = socket.clone();
            move |app| {
                let window = app.get_webview_window("main").unwrap();
                let recv_socket = socket.clone();

                // Spawn background thread for receiving
                thread::spawn(move || {
                    let mut buf = [0u8; 2048];

                    loop {
                        if let Ok(len) = recv_socket.recv(&mut buf) {
                            let data = buf[..len].to_vec();

                            // Send to frontend
                            window.emit("udp_packet", data).unwrap();
                        }
                    }
                });

                Ok(())
            }
        })
        .manage(UdpState { socket })
        .invoke_handler(tauri::generate_handler![send_packet, exit_app])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}