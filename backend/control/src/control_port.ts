import {createSocket} from "dgram";
import {CONTROL_PLANE_INT_PORT, DATA_PLANE_INT_PORT, GAME_MAX_PLAYERS, KEY_LEN} from "./config.js";

class CallBackMap extends Map<string, (buffer: Buffer) => void> {
    set(key: string, value: (buffer: Buffer) => void) : this {
        super.set(key, (buffer: Buffer) => {
            value(buffer);
            super.delete(key);
        });
        return this;
    }
}
class ControlPort {
    #client = createSocket("udp6");
    #callbacks : Array<CallBackMap | null> = [
        null,
        new Map()
    ];
    #get_key : Array<((buffer: Buffer) => string) | null> = [
        null,
        (buffer: Buffer) => buffer.subarray(1, 6).toString()
    ];
    constructor() {
        this.#client.on("error", err => console.error(err));
        this.#client.on("listening", () => {
            console.log("Control plane internal port listening on port %d.", CONTROL_PLANE_INT_PORT);
        });
        this.#client.on("message", (msg, rinfo) => {
            const cmd = msg.readUInt8(0);
            if (!this.#callbacks[cmd]) return;
            this.#callbacks[cmd].get(this.#get_key[cmd]!(msg))?.(msg);
        });
        this.#client.bind(CONTROL_PLANE_INT_PORT);
    }
    kill() {
        const buffer = new Uint8Array(1);
        buffer[0] = 0;
        this.#client.send(buffer, 0, buffer.length, DATA_PLANE_INT_PORT, "::1");
    }
    new_session(token: string, human_players: number, ai_players: number, keys: Uint8Array) {
        const buffer = new Uint8Array(1 + 5 + 1 + 1 + human_players * KEY_LEN);
        buffer[0] = 1;
        const enc = new TextEncoder();
        enc.encodeInto(token, buffer.subarray(1, 6));
        buffer[6] = human_players;
        buffer[7] = ai_players;
        let buffer_pos = 8, keys_pos = 0;
        for (let i = 0; i < human_players; i++) {
            buffer.set(keys.subarray(keys_pos, keys_pos + KEY_LEN), buffer_pos);
            buffer_pos += KEY_LEN;
            keys_pos += KEY_LEN;
        }
        const promise = new Promise<number>((res, rej) => {
            this.#callbacks[1]!.set(token, (buffer: Buffer) => {
                switch (buffer.readUInt8(6)) {
                    case 0: {
                        res(buffer.readUInt32LE(8));
                        break;
                    }
                    case 1: {
                        rej("Server busy. Please try again later.");
                        break;
                    }
                    case 2: {
                        rej(`The player count is too big. The maximum is ${GAME_MAX_PLAYERS}.`);
                        break;
                    }
                    default: {
                        rej("Unknown error.");
                        break;
                    }
                }
            });
        });
        this.#client.send(buffer, 0, buffer.length, DATA_PLANE_INT_PORT, "::1");
        return promise;
    }
}

export const control_port = new ControlPort();