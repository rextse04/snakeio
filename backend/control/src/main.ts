import {createServer} from "http";
import {CONTROL_PLANE_EXT_PORT, GAME_MAX_PLAYERS, KEY_LEN} from "./config.js";
import {type WebSocket, WebSocketServer} from "ws";
import {lobby, LobbyPlayer, LobbyRoom, PlayerRole} from "./lobby.js";
import type {Player} from "./player.js";
import {randomBytes} from "crypto";
import {control_port} from "./control_port.js";

const server = createServer();
server.on("error", err => console.error(err));
server.listen(CONTROL_PLANE_EXT_PORT);

interface PlayerSocket extends WebSocket {
    info?: Player
}
let player_count = 0;
const wss = new WebSocketServer({server});
function room_op_check(ws: PlayerSocket, msg: any, min_role: PlayerRole, allow_self = false)
    : [LobbyRoom, LobbyPlayer, LobbyPlayer] | undefined {
    const [room, player] = lobby.room_of(ws.info!.id) || [undefined, undefined];
    if (!room) {
        ws.send(JSON.stringify({
            ...msg,
            error: "You are not in a lobby room."
        }));
        return;
    }
    const object = room.players.find(player => player.info.id === msg.server_id);
    if (msg.server_id && !object) {
        ws.send(JSON.stringify({
            ...msg,
            error: "The player does not exist."
        }));
        return;
    }
    if (player!.role >= min_role || (allow_self && object!.info.id === ws.info!.id)) {
        return [room!, player!, object!];
    } else {
        ws.send(JSON.stringify({
            ...msg,
            error: "You do not have permission to perform this operation."
        }));
        return;
    }
}
function broadcast(player: Player, msg: any) {
    const room = lobby.room_of(player.id);
    if (room) {
        for (let player of room[0].players) {
            player.info.ws.send(msg);
        }
    } else {
        player.ws.send(msg);
    }
}
function broadcast_room(room: LobbyRoom, msg: any) {
    for (let player of room.players) {
        player.info.ws.send(msg);
    }
}
wss.on("error", err => console.error(err));
wss.on("listening", () => {
    console.log("Control plane external port listening on port %d.", CONTROL_PLANE_EXT_PORT);
});
wss.on("connection", (ws: PlayerSocket) => {
    ws.info = {
        ws: ws,
        id: player_count,
        username: "Player " + player_count++
    };
    ws.send(JSON.stringify({
        type: "register",
        server_id: ws.info!.id,
        username: ws.info!.username
    }));
    ws.on("message", raw_msg => {
        const msg = JSON.parse(raw_msg.toString());
        switch (msg.type) {
            case "register": {
                ws.info!.username = msg.username;
                broadcast(ws.info!, JSON.stringify({
                    ...msg,
                    server_id: ws.info!.id
                }));
                break;
            }
            case "room_new": {
                const token = lobby.new_room(new LobbyPlayer(ws.info!, PlayerRole.OWNER), msg.token);
                ws.send(JSON.stringify({
                    type: "room_new",
                    token: token.ok ? token.value : undefined,
                    error: token.ok ? undefined : token.error
                }));
                break;
            }
            case "room_join": {
                const room = msg.token === ""
                    ? (() => {
                        for (let room of lobby.values()) {
                            if (room.is_public && room.all_players < GAME_MAX_PLAYERS) {
                                return room;
                            }
                        }
                    })()
                    : lobby.room(msg.token);
                const error = room
                    ? room!.admit(new LobbyPlayer(ws.info!))
                    : (msg.token === "" ? "No available room found." : "Invalid room token.");
                const response = JSON.stringify({
                    ...msg,
                    ...room?.summary(),
                    error: error
                });
                if (error) broadcast(ws.info!, response);
                else broadcast_room(room!, response);
                break;
            }
            case "room_kick": {
                const result =
                    room_op_check(ws, msg, lobby.room_of(msg.server_id)?.[1].role!, true);
                if (result) {
                    const error = result[0].kick(msg.server_id);
                    if (error) {
                        ws.send(JSON.stringify({
                            ...msg,
                            error: error
                        }));
                    } else {
                        if (result[0].human_players === 0) {
                            lobby.delete(result[0].token);
                        } else {
                            broadcast_room(result[0], JSON.stringify({
                                type: "room_kick",
                                server_id: msg.server_id
                            }));
                        }
                        ws.send(raw_msg.toString());
                    }
                }
                break;
            }
            case "room_change_role": {
                const result =
                    room_op_check(ws, msg, PlayerRole.OWNER);
                if (result) {
                    if (result[0].started) {
                        ws.send(JSON.stringify({
                            ...msg,
                            error: "The session has already started."
                        }));
                    }
                    result[2].role = msg.role;
                    broadcast_room(result[0], raw_msg);
                }
                break;
            }
            case "room_set": {
                const result =
                    room_op_check(ws, msg, PlayerRole.OWNER);
                if (result) {
                    if (result[0].started) {
                        ws.send(JSON.stringify({
                            ...msg,
                            error: "The session has already started."
                        }));
                        break;
                    }
                    switch (msg.field) {
                        case "is_public": {
                            broadcast_room(result[0], JSON.stringify({
                                type: "room_set",
                                field: "is_public",
                                value: result[0].is_public = !!msg.value
                            }));
                            break;
                        }
                        case "ai_players": {
                            const ai_players = parseInt(msg.value);
                            if (isNaN(ai_players) || ai_players < 0) {
                                ws.send(JSON.stringify({
                                    ...msg,
                                    error: "Invalid ai_players."
                                }));
                            } else if (result[0].human_players + ai_players > GAME_MAX_PLAYERS) {
                                ws.send(JSON.stringify({
                                    ...msg,
                                    error: "The total number of players exceeds the maximum."
                                }));
                            } else {
                                result[0].ai_players = ai_players;
                                broadcast_room(result[0], JSON.stringify({
                                    type: "room_set",
                                    field: "ai_players",
                                    value: ai_players
                                }));
                            }
                            break;
                        }
                        default: {
                            ws.send(JSON.stringify({
                                ...msg,
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
                    room_op_check(ws, msg, PlayerRole.OWNER);
                if (!result) break;
                if (result[0].started) {
                    ws.send(JSON.stringify({
                        ...msg,
                        error: "The game has already started."
                    }));
                    break;
                }
                result[0].started = true;
                randomBytes(result[0].human_players * KEY_LEN, (err, buffer) => {
                    if (err) {
                        console.error(err);
                        broadcast_room(result[0], JSON.stringify({
                            ...msg,
                            error: "Failed to generate keys."
                        }));
                        return;
                    }
                    control_port.new_session(result[0].token, result[0].human_players, result[0].ai_players, buffer)
                        .then(session_id => {
                            for (let i = 0; i < result[0].players.length; i++) {
                                const player = result[0].players[i]!;
                                player.info.ws.send(JSON.stringify({
                                    type: "room_start",
                                    session_id: session_id,
                                    key: new TextDecoder().decode(buffer.subarray(i * KEY_LEN, (i+1) * KEY_LEN))
                                }));
                            }
                            lobby.remove_room(result[0].token);
                        })
                        .catch((error: string) => {
                            broadcast_room(result[0], JSON.stringify({
                                ...msg,
                                error: error
                            }));
                        });
                });
                break;
            }
        }
    });
    ws.on("close", () => {
        const room = lobby.room_of(ws.info!.id);
        room?.[0].kick(ws.info!.id);
        broadcast(ws.info!, JSON.stringify({
            type: "room_leave",
            server_id: ws.info!.id
        }));
    })
})