import React, {RefObject, useCallback, useContext, useEffect, useId, useRef, useState} from "react";
import {UIContext, GameContext, app_config} from "./App.tsx";
import {invoke} from "@tauri-apps/api/core";
import {Packet, packet_manager, PacketManager} from "./packet.ts";
import Game from "./Game.tsx";
import LobbyBackground from "./LobbyBackground.tsx";
import GameConfig from "./config.ts";
import {useStateRef} from "./utils.ts";

import "../css/Lobby.css";
import About from "./About.tsx";

export enum PlayerRole {
    MEMBER = 0,
    AI = 0.5,
    ADMIN = 1,
    OWNER = 2
}
export type Player = {
    server_id: number;
    username: string;
    role?: PlayerRole;
    connected?: boolean;
};
export type LobbyRoom = {
    token: string;
    players: Player[];
    is_public?: boolean;
    ai_players?: number;
    max_tick?: number;
};

export function allPlayers(room: LobbyRoom) {
    return room.players.length + (room.ai_players || 0);
}
export function usernameOf(room: LobbyRoom, player_id: number) {
    if (player_id < room.players.length) {
        return room.players[player_id].username;
    } else {
        return "AI " + (player_id - room.players.length + 1);
    }
}
function roleName(role: PlayerRole) {
    switch (role) {
        case PlayerRole.MEMBER: return "member";
        case PlayerRole.AI: return "AI";
        case PlayerRole.ADMIN: return "admin";
        case PlayerRole.OWNER: return "owner";
        default: return "unknown";
    }
}

function LobbyPlayer({wsRef, room, user, player, disabled = false}:
    {wsRef: RefObject<WebSocket | undefined>, room: LobbyRoom, user: Player, player: Player, disabled?: boolean}) {
    let icon = "fa-person-circle-question";
    switch (player.role) {
        case PlayerRole.AI: icon = "fa-robot"; break;
        case PlayerRole.MEMBER: icon = "fa-user"; break;
        case PlayerRole.ADMIN: icon = "fa-user-shield"; break;
        case PlayerRole.OWNER: icon = "fa-crown"; break;
    }
    const onRoleChange = () => {
        const target =
            (player.role === PlayerRole.ADMIN) ? PlayerRole.MEMBER : PlayerRole.ADMIN;

        if (!confirm("Are you sure you want to change " + player.username + "'s role to " + roleName(target) + "?"))
            return;
        wsRef.current?.send(JSON.stringify({
            type: "room_change_role",
            server_id: player.server_id,
            role: target
        }));
    };
    const onKick = () => {
        const confirm_msg = user.server_id === player.server_id
            ? "Are you sure you want to leave the room?"
            : "Are you sure you want to kick " + player.username + "?";
        if (!confirm(confirm_msg)) return;
        if (player.role === PlayerRole.AI) {
            wsRef.current?.send(JSON.stringify({
                type: "room_set",
                field: "ai_players",
                value: room.ai_players! - 1
            }));
        } else {
            wsRef.current?.send(JSON.stringify({
                type: "room_kick",
                server_id: player.server_id
            }));
        }
    };
    const kickable = user.server_id !== player.server_id && user.role! < player.role!;
    const usernameClasses = ["username"];
    if (player.connected !== undefined) {
        usernameClasses.push(player.connected ? "connected" : "disconnected");
    }
    return <div>
        <button className="icon" disabled={disabled || player.role === PlayerRole.AI || player.role === PlayerRole.OWNER}
            onClick={onRoleChange}>
            <i className={"fa-solid " + icon}></i>
        </button>
        <span className={usernameClasses.join(" ")}>{player.username}</span>
        <div className="flex-spacer"></div>
        <button className="icon" disabled={disabled || kickable} onClick={onKick}>
            <i className="fa-solid fa-xmark"></i>
        </button>
    </div>;
}
export default function Lobby() {
    const [, setGame] = useContext(GameContext);
    const [, setUI] = useContext(UIContext);
    const [gameConfig, gameConfigRef, setGameConfig] = useStateRef<GameConfig | undefined>(undefined);
    const [player, setPlayer] = useState({
        server_id: -1,
        username: ""
    } as Player);
    const [room, setRoom] = useState({
        token: "",
        players: []
    } as LobbyRoom);
    const [started, setStarted] = useState(false);
    const roomRef = useRef(room);

    useEffect(() => {
        setGame(<LobbyBackground />);
    }, []);

    const wsRef = useRef(undefined as WebSocket | undefined);
    const connect = useCallback(() => {
        const ws = new WebSocket(app_config.control_server_addr);
        ws.addEventListener("error", error => {
            console.error(error);
            if (confirm("Failed to connect to the server. Retry?")) {
                connect();
            } else {
                invoke("exit_app");
            }
        });
        ws.addEventListener("message", raw_msg => {
            const msg = JSON.parse(raw_msg.data);
            if (msg.error) {
                alert(msg.error);
                return;
            }
            switch (msg.type) {
                case "config": {
                    setGameConfig(msg.config);
                    break;
                }
                case "register": {
                    setPlayer(player => ({
                        ...player,
                        server_id: msg.server_id,
                        username: msg.username
                    }));
                    setRoom(room => {
                        const teammate =
                            room.players.find(player => player.server_id === msg.server_id);
                        if (teammate) {
                            teammate.username = msg.username;
                            return {...room};
                        } else {
                            return room;
                        }
                    });
                    break;
                }
                case "room_new": {
                    setPlayer(player => {
                        const new_player = {
                            ...player,
                            role: PlayerRole.OWNER
                        };
                        setRoom({
                            token: msg.token,
                            players: [new_player],
                            is_public: false,
                            ai_players: 0
                        });
                        return new_player;
                    });
                    break;
                }
                case "room_join": {
                    setRoom(msg);
                    break;
                }
                case "room_kick": {
                    setPlayer(player => {
                        if (msg.server_id === player.server_id) {
                            setRoom({token: "", players: []});
                        } else {
                            setRoom(room => ({
                                ...room,
                                players: room.players.filter(player => player.server_id !== msg.server_id)
                            }));
                        }
                        return player;
                    });
                    break;
                }
                case "room_change_role": {
                    setPlayer(player => {
                        if (msg.server_id === player.server_id) {
                            return {...player, role: msg.role};
                        } else {
                            const teammate =
                                room.players.find(player => player.server_id === msg.server_id);
                            if (teammate) {
                                teammate.role = msg.role;
                                setRoom(room => ({...room}));
                            }
                            return player;
                        }
                    });
                    break;
                }
                case "room_set": {
                    setRoom(room => ({...room, [msg.field]: msg.value}));
                    break;
                }
                case "room_start": {
                    const error =
                        packet_manager.set(gameConfigRef.current!.data_server_addr,
                            msg.session_id, msg.player_id, Uint8Array.from(msg.key));
                    if (error) {
                        alert("Internal server error: " + error);
                        break;
                    }
                    const keep_alive = () => {
                        const packet = new ArrayBuffer(PacketManager.align(8));
                        const view = new DataView(packet);
                        view.setUint8(0, 1);
                        view.setFloat32(4, NaN, true);
                        packet_manager.send(new Uint8Array(packet));
                    };
                    const listener = ({tick, data}: Packet) => {
                        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
                        if (view.getUint32(0, true) != 2) {
                            setUI(undefined);
                            setGame(<Game config={gameConfigRef.current!}
                                          room={roomRef.current!} first_packet={{tick, data}}/>)
                            packet_manager.remove_listener(listener);
                            return;
                        }
                        setRoom(room => {
                            for (const player of room.players) {
                                player.connected = view.getUint8(4 + room.players.indexOf(player)) === 1;
                            }
                            return {...room};
                        });
                        keep_alive();
                    };
                    packet_manager.add_listener(listener);
                    keep_alive();
                    setStarted(true);
                    break;
                }
            }
        });
        ws.addEventListener("open", () => wsRef.current = ws);
    }, []);
    useEffect(() => {
        connect();
        return () => wsRef.current?.close();
    }, []);

    const disabled = !gameConfig || started;
    const session_token_id = useId();
    const max_tick_id = useId();
    const onUsernameChange = (e: any) => {
        setPlayer(player => ({
            ...player,
            username: e.target.value
        }));
    };
    const onRegister = () => {
        wsRef.current?.send(JSON.stringify({
            type: "register",
            username: player.username
        }));
    };
    const onTokenChange = (e: any) => {
        setRoom(room => ({
            ...room,
            token: e.target.value
        }));
    };
    const onJoin = () => {
        wsRef.current?.send(JSON.stringify({
            type: "room_join",
            token: room.token
        }));
    };
    const onCreate = () => {
        wsRef.current?.send(JSON.stringify({
            type: "room_new",
            token: room.token
        }));
    };
    const onPublicChange = (e: any) => {
        wsRef.current?.send(JSON.stringify({
            type: "room_set",
            field: "is_public",
            value: e.target.checked
        }));
    };
    const onAiPlayersChange = () => {
        wsRef.current?.send(JSON.stringify({
            type: "room_set",
            field: "ai_players",
            value: room.ai_players! + 1
        }));
    };
    const onMaxTickChange = (e: any) => {
        wsRef.current?.send(JSON.stringify({
            type: "room_set",
            field: "max_tick",
            value: e.target.value * 1000 / gameConfigRef.current!.tick_rate_ms
        }));
    }
    const onStart = () => {
        roomRef.current = room;
        wsRef.current?.send(JSON.stringify({
            type: "room_start"
        }));
    };
    return <>
        <div className="lobby">
            <div className="row-input">
                <label htmlFor="username">Username</label>
                <input className="main username" type="text" value={player.username} onChange={onUsernameChange} />
                <button className="register" onClick={onRegister}>Register</button>
            </div>
            <div className="divider"></div>
            <div className="main">
                <div className="row-input">
                    <label htmlFor={session_token_id}>Session Token</label>
                    <input type="text" id={session_token_id} className="session-token main validated"
                           minLength={5} maxLength={5} pattern="[A-Za-z0-9]{5}"
                           title="A session token must consist of 5 alphanumeric characters."
                           value={room.token} onChange={onTokenChange}
                           readOnly={disabled || room.players.length > 0} />
                    <button disabled={disabled || room.players.length > 0} onClick={onJoin}>Join</button>
                    <button disabled={disabled || room.players.length > 0} onClick={onCreate}>Create</button>
                </div>
                {room.players.length > 0 && <>
                    <div className="lobby-room">
                        <div>
                            <input type="checkbox" checked={room.is_public} onChange={onPublicChange}
                                   disabled={disabled || player.role! < PlayerRole.ADMIN} />
                            <label htmlFor="lobby-room-public">Public</label>
                            <div className="flex-spacer"></div>
                            <button className="icon" onClick={onAiPlayersChange}
                                    disabled={disabled || player.role! < PlayerRole.ADMIN}>
                                <i className="fa-solid fa-robot"></i>
                            </button>
                        </div>
                        {room.players.map(teammate => (
                            <LobbyPlayer key={teammate.server_id}
                                         wsRef={wsRef} room={room} user={player} player={teammate} disabled={disabled} />
                        ))}
                        {[...function*() {
                            for (let player_id = room.players.length; player_id < allPlayers(room); ++player_id) {
                                yield <LobbyPlayer key={player_id} wsRef={wsRef} user={player} room={room} player={{
                                    server_id: -1,
                                    username: usernameOf(room, player_id),
                                    role: PlayerRole.AI
                                }} disabled={disabled} />;
                            }
                        }()]}
                    </div>
                    <div className="row-input">
                        <label htmlFor={max_tick_id}>Game Time (s)</label>
                        <input type="number" id={max_tick_id}
                               min={0} max={gameConfig!.game_max_tick * gameConfig!.tick_rate_ms / 1000} step={15}
                               disabled={disabled || player.role! < PlayerRole.ADMIN}
                               value={(room.max_tick || gameConfig!.game_max_tick) * gameConfig!.tick_rate_ms / 1000}
                               onChange={onMaxTickChange} />
                    </div>
                </>}
            </div>
            <div className="divider"></div>
            <button className="start-btn" disabled={disabled || player.role !== PlayerRole.OWNER} onClick={onStart}>
                {started ? "Waiting..." : "Start!"}
            </button>
        </div>
        <div className="about">
            <button className="icon-button" onClick={() => setUI(<About />)}>
                <i className="fa-solid fa-circle-info"></i>
            </button>
        </div>
    </>;
}