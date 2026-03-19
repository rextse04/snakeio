import sodium from "libsodium-wrappers";
import {invoke} from "@tauri-apps/api/core";
import {listen} from "@tauri-apps/api/event";
import {PACKET_CHUNK_SIZE} from "./config.ts";

await sodium.ready;

export type Packet = {
    tick: number;
    data: Uint8Array;
};
export class PacketManager {
    #addr = "";
    #session_id = -1;
    #player_id = -1;
    #key = new Uint8Array();
    #counter = 0;
    #tick = -1;
    #buffer: Uint8Array | undefined = undefined;
    #recv = 0;
    #listeners = new Set<(packet: Packet) => void>();
    constructor() {
        listen("recv_packet", this.#listener);
    }
    set(addr: string, session_id: number, player_id: number, key: Uint8Array) {
        this.#addr = addr;
        if (key.length !== 32) {
            return "Invalid key length.";
        }
        this.#session_id = session_id;
        this.#player_id = player_id;
        this.#key = key;
        this.#counter = 0;
        this.#tick = -1;
        this.#buffer = undefined;
        this.#listeners = new Set();
    }
    get session_id() {
        return this.#session_id;
    }
    get player_id() {
        return this.#player_id;
    }
    get key() {
        return this.#key;
    }
    get counter() {
        return this.#counter;
    }
    get tick() {
        return this.#tick;
    }
    static align(size: number) {
        return Math.ceil(size / 16) * 16;
    }
    // data.length must be a multiple of 64
    send(data: Uint8Array) {
        const aad = new ArrayBuffer(16);
        const view = new DataView(aad);
        view.setUint32(0, this.#session_id, true);
        view.setUint32(4, this.#player_id, true);
        view.setUint8(9, 1);
        view.setUint8(10, 1);
        view.setUint32(12, this.#counter, true);
        const nonce = new Uint8Array(view.buffer, 4, 12);
        const {ciphertext, mac} = sodium.crypto_aead_chacha20poly1305_ietf_encrypt_detached(
            data, new Uint8Array(aad), null, nonce, this.#key);
        const packet = new Uint8Array(aad.byteLength + ciphertext.byteLength + mac.byteLength);
        packet.set(new Uint8Array(aad), 0);
        packet.set(ciphertext, aad.byteLength);
        packet.set(mac, aad.byteLength + ciphertext.byteLength);
        this.#counter++;
        return invoke("send_packet", {addr: this.#addr, data: packet});
    }
    #listener= (event: any) => {
        const packet = new Uint8Array(event.payload as ArrayBuffer);
        const view = new DataView(packet.buffer);
        const session_id = view.getUint32(0, true);
        const player_id = view.getUint32(4, true);
        const tick = view.getUint32(12, true);
        if (session_id != this.#session_id || player_id != this.#player_id || tick < this.#tick) return;
        try {
            const decrypted = sodium.crypto_aead_chacha20poly1305_ietf_decrypt_detached(
                null,
                packet.subarray(16, packet.length - 16),
                packet.subarray(packet.length - 16, packet.length),
                packet.subarray(0, 16),
                packet.subarray(4, 16),
                this.#key);
            const total_chunks = view.getUint8(9);
            const chunk_id = view.getUint8(10);
            if (tick > this.#tick) {
                this.#tick = tick;
                this.#buffer = new Uint8Array(total_chunks * PACKET_CHUNK_SIZE);
                this.#recv = 0;
            }
            this.#buffer!.set(decrypted, chunk_id * PACKET_CHUNK_SIZE);
            if (++this.#recv == total_chunks) {
                this.#listeners.forEach(listener => listener({tick, data: this.#buffer!}));
            }
        } catch (e) {
            console.error(e, packet);
        }
    }
    add_listener(listener: (packet: Packet) => void) {
        this.#listeners.add(listener);
    }
    remove_listener(listener: (packet: Packet) => void) {
        this.#listeners.delete(listener);
    }
}

export const packet_manager = new PacketManager();