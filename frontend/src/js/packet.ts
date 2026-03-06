import sodium from "libsodium-wrappers";
import {invoke} from "@tauri-apps/api/core";
import {listen} from "@tauri-apps/api/event";

await sodium.ready;

export class PacketManager {
    #session_id = -1;
    #player_id = -1;
    #key = new Uint8Array();
    #counter = 0;
    set(session_id: number, player_id: number, key: Uint8Array) {
        if (key.length !== 32) {
            return "Invalid key length.";
        }
        this.#session_id = session_id;
        this.#player_id = player_id;
        this.#key = key;
        this.#counter = 0;
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
    static align(size: number) {
        return Math.ceil(size / 16) * 16;
    }
    // data.length must be a multiple of 64
    send(data: Uint8Array) {
        const aad = new ArrayBuffer(16);
        const view = new DataView(aad);
        view.setUint32(0, this.#session_id, true);
        view.setUint32(4, this.#player_id, true);
        // view.setUint32(8, 0, true);
        view.setUint32(12, this.#counter, true);
        const nonce = new Uint8Array(view.buffer, 4, 12);
        const {ciphertext, mac} = sodium.crypto_aead_chacha20poly1305_ietf_encrypt_detached(
            data, new Uint8Array(aad), null, nonce, this.#key);
        const packet = new Uint8Array(aad.byteLength + ciphertext.byteLength + mac.byteLength);
        packet.set(new Uint8Array(aad), 0);
        packet.set(ciphertext, aad.byteLength);
        packet.set(mac, aad.byteLength + ciphertext.byteLength);
        this.#counter++;
        return invoke("send_packet", packet);
    }
    onrecv(handler: (data: Uint8Array) => void) {
        return listen("recv_packet", (event) => {
            const packet = new Uint8Array(event.payload as ArrayBuffer);
            handler(sodium.crypto_aead_chacha20poly1305_ietf_decrypt_detached(
                null,
                packet.subarray(16, packet.length - 16),
                packet.subarray(packet.length - 16, packet.length),
                packet.subarray(0, 16),
                packet.subarray(4, 16),
                this.#key));
        });
    }
}

export const packet_manager = new PacketManager();
packet_manager.onrecv(data => console.log(data));