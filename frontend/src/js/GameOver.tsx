import React from "react";
import GameControl from "./GameControl.tsx";

export default function GameOver({score}: {score: number}) {
    return <div className="game-over">
        <h1>You Lost 😞</h1>
        <div>Your score: {score}</div>
        <div className="divider"></div>
        <GameControl />
    </div>;
}