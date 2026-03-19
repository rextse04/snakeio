import {GAME_MAX_PLAYERS, GAME_MAX_TICK, SESSION_TOKEN_LEN} from "./config.js";
import type {Player} from "./player.js";
import type {Result} from "./utils.js";

export enum PlayerRole {
    MEMBER = 0,
    ADMIN = 1,
    OWNER = 2
}
export interface LobbyPlayerSummary {
    id: number;
    username: string;
    permission: PlayerRole;
}
export class LobbyPlayer {
    info: Player;
    role: PlayerRole;
    constructor(info: Player, permission = PlayerRole.MEMBER) {
        this.info = info;
        this.role = permission;
    }
    summary(): LobbyPlayerSummary {
        return {
            id: this.info.id,
            username: this.info.username,
            permission: this.role
        }
    }
}

export interface LobbyRoomSummary {
    token: string;
    players: LobbyPlayerSummary[];
    is_public: boolean;
    ai_players: number;
    max_tick: number;
}
export class LobbyRoom {
    #room_of: Map<number, [LobbyRoom, LobbyPlayer]>;
    token: string;
    players: Array<LobbyPlayer>;
    is_public: boolean;
    ai_players: number;
    max_tick: number;
    started = false;
    constructor(room_of: Map<number, [LobbyRoom, LobbyPlayer]>, token: string, players: Array<LobbyPlayer>,
                is_public = false, ai_players: number = 0, max_tick = GAME_MAX_TICK) {
        this.#room_of = room_of;
        this.token = token;
        this.players = players;
        this.is_public = is_public;
        this.ai_players = ai_players;
        this.max_tick = max_tick;
    }
    get human_players() {
        return this.players.length;
    }
    get all_players() {
        return this.human_players + this.ai_players;
    }
    admit(player: LobbyPlayer) {
        if (this.started) return "The session has already started.";
        if (this.#room_of.has(player.info.id)) return "You are already in a session.";
        if (this.players.length + this.ai_players >= GAME_MAX_PLAYERS) return "The room is full.";
        this.players.push(player);
        this.#room_of.set(player.info.id, [this, this.players.at(-1)!]);
    }
    kick(server_id: number) {
        if (this.started) return "The session has already started.";
        const player_id = this.players.findIndex(player => player.info.id === server_id);
        if (player_id === -1) return "The player does not exist in the room.";
        this.players.splice(player_id);
        this.#room_of.delete(server_id);
    }
    summary(): LobbyRoomSummary {
        return {
            token: this.token,
            players: this.players.map(player => player.summary()),
            is_public: this.is_public,
            ai_players: this.ai_players,
            max_tick: this.max_tick
        }
    }
}

class Lobby extends Map<string, LobbyRoom> {
    characters = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
    #room_of: Map<number, [LobbyRoom, LobbyPlayer]> = new Map();
    new_room(owner: LobbyPlayer, token = ""): Result<string, string> {
        if (token === "") {
            for (let i = 0; i < SESSION_TOKEN_LEN; i++) {
                token += this.characters.charAt(Math.floor(Math.random() * this.characters.length));
            }
            if (this.has(token)) return {
                ok: false,
                error: "Unable to generate token. Please try again later."
            };
        } else if (!/[0-9a-zA-Z]{5}/.test(token)) {
            return {
                ok: false,
                error: "Invalid session token. Tokens must consist of 5 alphanumeric characters."
            };
        }
        const room = new LobbyRoom(this.#room_of, token, [owner]);
        this.set(token, room);
        this.#room_of.set(owner.info.id, [room, room.players[0]!]);
        return {
            ok: true,
            value: token
        };
    }
    remove_room(token: string) {
        const room = this.get(token);
        if (!room) return "The room does not exist.";
        this.delete(token);
        for (const player of room.players) {
            this.#room_of.delete(player.info.id);
        }
    }
    room(token: string) {
        return this.get(token);
    }
    room_of(server_id: number) {
        return this.#room_of.get(server_id);
    }
}
export const lobby = new Lobby();