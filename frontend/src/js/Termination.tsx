import React, {useMemo} from "react";
import {LobbyRoom, usernameOf} from "./Lobby.tsx";
import GameControl from "./GameControl.tsx";
import {SnakeBasic, SnakeStatus, SnakeStatusType} from "./engine.ts";

function rankText(rank: number) {
    switch (rank) {
        case 1: return "🥇";
        case 2: return "🥈";
        case 3: return "🥉";
        default: return rank.toString();
    }
}
export function killReasonText(room: LobbyRoom, status: SnakeStatus) {
    switch (status.status) {
        case SnakeStatusType.KILLED_BY_SNAKE: return `Killed by ${usernameOf(room, status.data)}`;
        case SnakeStatusType.KILLED_BY_WALL: return `Killed by wall`;
        default: return "";
    }
}
function statusText(room: LobbyRoom, status: SnakeStatus) {
    if (status.status === SnakeStatusType.ALIVE) {
        return "✅️"
    } else {
        return "❌️: " + killReasonText(room, status);
    }
}
export default function Termination({room, snakes}: {room: LobbyRoom, snakes: SnakeBasic[]}) {
    const basics = useMemo(() => {
        const out = snakes.map(
            (basic, idx) => ({...basic, player_id: idx})
        );
        return out.sort((a, b) => b.score - a.score);
    }, [snakes]);
    return <div className="termination">
        <h1>Game Over!</h1>
        <table className="scoreboard">
            <thead>
            <tr>
                <th>Rank</th>
                <th>Name</th>
                <th>Score</th>
                <th>Alive</th>
            </tr>
            </thead>
            <tbody>
            {basics.map((snake, idx) => <tr key={idx}>
                <td>{rankText(idx+1)}</td>
                <td>{usernameOf(room, snake.player_id)}</td>
                <td>{snake.score}</td>
                <td>{statusText(room, snake.status)}</td>
            </tr>)}
            </tbody>
        </table>
        <div className="divider"></div>
        <GameControl />
    </div>;
}