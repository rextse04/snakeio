import {GAME_MAX_PLAYERS, KEY_LEN, SESSION_TOKEN_LEN} from "./config.js";
import {randomBytes} from "node:crypto";
import type {Player} from "./player.js";

export enum PlayerPermission {
    MEMBER = 0,
    ADMIN = 1,
    OWNER = 2
}
export type LobbyPlayer = {
    info: Player,
    permission: PlayerPermission
}

export class LobbyRoom {
    #room_of: Map<number, [LobbyRoom, LobbyPlayer]>;
    players: Array<LobbyPlayer>;
    ai_players: number;
    started = false;
    constructor(room_of: Map<number, [LobbyRoom, LobbyPlayer]>, players: Array<LobbyPlayer>, ai_players: number = 0) {
        this.#room_of = room_of;
        this.players = players;
        this.ai_players = ai_players;
    }
    get human_players() {
        return this.players.length - this.ai_players;
    }
    admit(player: LobbyPlayer) {
        if (this.started) return false;
        if (this.#room_of.has(player.info.id)) return false;
        if (this.players.length + this.ai_players >= GAME_MAX_PLAYERS) return false;
        this.players.push(player);
        this.#room_of.set(player.info.id, [this, this.players.at(-1)!]);
        return true;
    }
    kick(server_id: number) {
        if (this.started) return false;
        const player_id = this.players.findIndex(player => player.info.id === server_id);
        if (player_id === -1) return false;
        this.players.splice(player_id);
        this.#room_of.delete(server_id);
        return true;
    }
}

class Lobby extends Map<string, LobbyRoom> {
    characters = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
    #room_of: Map<number, [LobbyRoom, LobbyPlayer]> = new Map();
    new_room(owner: LobbyPlayer) {
        let token = "";
        for (let i = 0; i < SESSION_TOKEN_LEN; i++) {
            token += this.characters.charAt(Math.floor(Math.random() * this.characters.length));
        }
        if (this.has(token)) return undefined;
        const room = new LobbyRoom(this.#room_of, [owner]);
        this.set(token, room);
        this.#room_of.set(owner.info.id, [room, room.players[0]!]);
        return token;
    }
    room(token: string) {
        return this.get(token);
    }
    room_of(server_id: number) {
        return this.#room_of.get(server_id);
    }
}
export const lobby = new Lobby();