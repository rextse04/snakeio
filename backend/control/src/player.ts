import type {WebSocket} from "ws";

export type Player = {
    ws: WebSocket;
    id: number;
    username: string;
}