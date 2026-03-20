import React from "react";
import GameControl from "./GameControl.tsx";
import {SnakeBasic} from "./engine.ts";
import {killReasonText} from "./Termination.tsx";
import {LobbyRoom} from "./Lobby.tsx";

export default function GameOver({room, snake}: {room: LobbyRoom, snake: SnakeBasic}) {
    return <div className="game-over">
        <h1>You Lost 😞</h1>
        <div><i>{killReasonText(room, snake.status)}</i></div>
        <div>Your score: <b>{snake.score}</b></div>
        <div className="divider"></div>
        <GameControl />
    </div>;
}