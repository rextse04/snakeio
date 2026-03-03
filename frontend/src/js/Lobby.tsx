import React, {RefObject, useContext, useEffect, useId, useRef, useState} from "react";
import {UIContext} from "./App.tsx";
import "../css/Lobby.css";
import {invoke} from "@tauri-apps/api/core";

enum PlayerRole {
    MEMBER = 0,
    AI = 0.5,
    ADMIN = 1,
    OWNER = 2
}
type Player = {
    server_id: number;
    username: string;
    role?: PlayerRole;
};
type LobbyRoom = {
    token: string;
    players: Array<Player>;
    is_public?: boolean;
    ai_players?: number;
};

function role_name(role: PlayerRole) {
    switch (role) {
        case PlayerRole.MEMBER: return "member";
        case PlayerRole.AI: return "AI";
        case PlayerRole.ADMIN: return "admin";
        case PlayerRole.OWNER: return "owner";
        default: return "unknown";
    }
}
function LobbyPlayer({wsRef, room, user, player}:
                     {wsRef: RefObject<WebSocket | undefined>, room: LobbyRoom, user: Player, player: Player}) {
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
        if (!confirm("Are you sure you want to change " + player.username + "'s role to " + role_name(target) + "?"))
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
    return <div>
        <button className="icon" disabled={player.role === PlayerRole.AI || player.role === PlayerRole.OWNER}
            onClick={onRoleChange}>
            <i className={"fa-solid " + icon}></i>
        </button>
        <span className="username">{player.username}</span>
        <div className="flex-spacer"></div>
        <button className="icon" disabled={kickable} onClick={onKick}>
            <i className="fa-solid fa-xmark"></i>
        </button>
    </div>;
}
export default function Lobby() {
    const [UI, setUI] = useContext(UIContext);
    const [player, setPlayer] = useState({
        server_id: -1,
        username: ""
    } as Player);
    const [room, setRoom] = useState({
        token: "",
        players: []
    } as LobbyRoom);
    const wsRef = useRef(undefined as WebSocket | undefined);
    const connect = () => {
        const ws = new WebSocket("ws://localhost:50000");
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
            console.debug(msg);
            if (msg.error) {
                alert(msg.error);
                return;
            }
            switch (msg.type) {
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
                    if (msg.server_id === player.server_id) {
                        setRoom({token: "", players: []});
                    } else {
                        setRoom(room => ({
                            ...room,
                            players: room.players.filter(player => player.server_id !== msg.server_id)
                        }));
                    }
                    break;
                }
                case "room_change_role": {
                    if (msg.server_id === player.server_id) {
                        setPlayer(player => ({...player, role: msg.role}));
                    }
                    const teammate =
                        room.players.find(player => player.server_id === msg.server_id);
                    if (teammate) {
                        teammate.role = msg.role;
                        setRoom(room => ({...room}));
                    }
                    break;
                }
                case "room_set": {
                    setRoom(room => ({...room, [msg.field]: msg.value}));
                    break;
                }
                case "room_start": {
                    setUI(null);
                    break;
                }
            }
        });
        ws.addEventListener("open", () => wsRef.current = ws);
    };
    useEffect(connect, []);
    const session_token_id = useId();
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
    const onStart = () => {
        wsRef.current?.send(JSON.stringify({
            type: "room_start"
        }));
    };
    const ai_players = [];
    for (let i = 1; i <= (room.ai_players ? room.ai_players : 0); ++i) {
        ai_players.push(<LobbyPlayer key={"ai-" + i} wsRef={wsRef} user={player} room={room} player={{
            server_id: -1,
            username: "AI " + i,
            role: PlayerRole.AI
        }} />);
    }
    return <div className="lobby">
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
                       readOnly={room.players.length > 0} />
                <button disabled={room.players.length > 0} onClick={onJoin}>Join</button>
                <button disabled={room.players.length > 0} onClick={onCreate}>Create</button>
            </div>
            {room.players.length > 0 && <div className="lobby-room">
                <div>
                    <input type="checkbox" checked={room.is_public} onChange={onPublicChange} />
                    <label htmlFor="lobby-room-public">Public</label>
                    <div className="flex-spacer"></div>
                    <button className="icon" onClick={onAiPlayersChange}>
                        <i className="fa-solid fa-robot"></i>
                    </button>
                </div>
                {room.players.map(teammate => (
                    <LobbyPlayer key={teammate.server_id} wsRef={wsRef} room={room} user={player} player={teammate} />
                ))}
                {ai_players}
            </div>}
        </div>
        <div className="divider"></div>
        <button className="start-btn" disabled={player.role !== PlayerRole.OWNER} onClick={onStart}>Start!</button>
    </div>;
}