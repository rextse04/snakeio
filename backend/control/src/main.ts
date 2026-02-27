import {createServer} from "node:https";
import {CONTROL_PLANE_EXT_PORT, KEY_LEN} from "./config.js";
import {type WebSocket, WebSocketServer} from "ws";
import {lobby, type LobbyPlayer, LobbyRoom, PlayerPermission} from "./lobby.js";
import type {Player} from "./player.js";
import {randomBytes} from "node:crypto";
import {control_port} from "./control_port.js";

const server = createServer({
    key: process.env.KEY,
    cert: process.env.CERT
});
server.on("error", err => console.error(err));
server.listen(CONTROL_PLANE_EXT_PORT);

interface PlayerSocket extends WebSocket {
    info?: Player
}
let player_count = 0;
const wss = new WebSocketServer({server});
function room_op_check(ws: PlayerSocket, object_player_id: number, min_permission: PlayerPermission, allow_self = false)
    : [LobbyRoom, LobbyPlayer, LobbyPlayer] | undefined {
    const [room, player] = lobby.room_of(ws.info!.id) || [undefined, undefined];
    if (!room) {
        ws.send(JSON.stringify({
            success: false,
            error: "You are not in a lobby room."
        }));
        return;
    }
    const object = room.players[object_player_id];
    if (!object) {
        ws.send(JSON.stringify({
            success: false,
            error: "The player does not exist."
        }));
        return;
    }
    if (player!.permission >= min_permission || (allow_self && object!.info.id === ws.info!.id)) {
        return [room!, player!, object!];
    } else {
        ws.send(JSON.stringify({
            success: false,
            error: "You are not an admin of the lobby room."
        }));
        return;
    }
}
wss.on("error", err => console.error(err));
wss.on("listening", () => {
    console.log("Control plane external port listening on port %d.", CONTROL_PLANE_EXT_PORT);
});
wss.on("connection", (ws: PlayerSocket) => {
    ws.info = {
        ws: ws,
        id: player_count++,
        username: "Player " + player_count
    };
    ws.on("message", raw_msg => {
        const msg = JSON.parse(raw_msg.toString());
        switch (msg.type) {
            case "register": {
                ws.info!.username = msg.username;
                ws.send(JSON.stringify({
                    success: true
                }));
                break;
            }
            case "room_new": {
                const token = lobby.new_room({
                    info: ws.info!,
                    permission: PlayerPermission.OWNER
                });
                if (token) {
                    ws.send(JSON.stringify({
                        success: true,
                        token: token
                    }));
                } else {
                    ws.send(JSON.stringify({
                        success: false,
                        error: "Session token generation failed. Please try again later."
                    }));
                }
                break;
            }
            case "room_join": {
                if (lobby.room(msg.token)?.admit({
                    info: ws.info!,
                    permission: PlayerPermission.MEMBER
                })) {
                    ws.send(JSON.stringify({
                        success: true
                    }));
                } else {
                    ws.send(JSON.stringify({
                        success: false,
                    }));
                }
                break;
            }
            case "room_kick": {
                const result =
                    room_op_check(ws, msg.player_id, PlayerPermission.ADMIN, true);
                if (result) {
                    ws.send(JSON.stringify({
                        success: result[0].kick(msg.server_id),
                        error: "Invalid server_id." // ignored if success is true
                    }));
                }
                break;
            }
            case "room_promote": {
                const result =
                    room_op_check(ws, msg.player_id, PlayerPermission.OWNER);
                if (result) {
                    result[2].permission = PlayerPermission.ADMIN;
                    ws.send(JSON.stringify({
                        success: true
                    }));
                }
                break;
            }
            case "room_demote": {
                const result =
                    room_op_check(ws, msg.player_id, PlayerPermission.ADMIN);
                if (result) {
                    result[2].permission = PlayerPermission.MEMBER;
                    ws.send(JSON.stringify({
                        success: true
                    }));
                }
                break;
            }
            case "room_set": {
                const result =
                    room_op_check(ws, msg.player_id, PlayerPermission.OWNER);
                if (result) {
                    switch (msg.field) {
                        case "ai_players": {
                            result[0].ai_players = msg.value;
                            ws.send(JSON.stringify({
                                success: true
                            }));
                            break;
                        }
                        default: {
                            ws.send(JSON.stringify({
                                success: false,
                                error: "Invalid field."
                            }));
                            break;
                        }
                    }
                }
                break;
            }
            case "room_start": {
                const result =
                    room_op_check(ws, msg.player_id, PlayerPermission.OWNER);
                if (!result) break;
                if (result[0].started) {
                    ws.send(JSON.stringify({
                        success: false,
                        error: "The game has already started."
                    }));
                    break;
                }
                result[0].started = true;
                for (let player of result[0].players) {
                    player.info.ws.send(JSON.stringify({
                        type: "room_start"
                    }));
                }
                randomBytes(result[0].human_players * KEY_LEN, (err, buffer) => {
                    if (err) {
                        console.error(err);
                        ws.send(JSON.stringify({
                            success: false,
                            error: "Failed to generate keys."
                        }));
                        return;
                    }
                    control_port.new_session(msg.token, result[0].human_players, result[0].ai_players, buffer)
                        .then(session_id => {
                            for (let i = 0; i < result[0].players.length; i++) {
                                const player = result[0].players[i]!;
                                player.info.ws.send(JSON.stringify({
                                    success: true,
                                    session_id: session_id,
                                    key: new TextDecoder().decode(buffer.subarray(i * KEY_LEN, (i+1) * KEY_LEN))
                                }));
                            }
                        })
                        .catch((err: string) => {
                            for (let player of result[0].players) {
                                player.info.ws.send(JSON.stringify({
                                    success: false,
                                    error: err
                                }));
                            }
                        });
                });
                break;
            }
        }
    });
    ws.on("close", (ws: PlayerSocket) => {
        const room = lobby.room_of(ws.info!.id);
        room?.[0].kick(ws.info!.id);
        room?.[0].players.forEach(player => {
            player.info.ws.send(JSON.stringify({
                type: "room_leave",
                server_id: player.info.id
            }));
        })
    })
})