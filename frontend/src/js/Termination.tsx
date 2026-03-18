import React, {useMemo} from "react";
import {SnakeBasic} from "./Game.tsx";
import {LobbyRoom, usernameOf} from "./Lobby.tsx";
import GameControl from "./GameControl.tsx";

function rank_text(rank: number) {
    switch (rank) {
        case 1: return "🥇";
        case 2: return "🥈";
        case 3: return "🥉";
        default: return rank.toString();
    }
}
export default function Termination({room, basics}: {room: LobbyRoom, basics: SnakeBasic[]}) {
    const basics_ = useMemo(() => {
        const out = basics.map(
            (basic, idx) => ({...basic, player_id: idx})
        );
        return out.sort((a, b) => b.score - a.score);
    }, [basics]);
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
            {basics_.map((basic, idx) => <tr key={idx}>
                <td>{rank_text(idx+1)}</td>
                <td>{usernameOf(room, basic.player_id)}</td>
                <td>{basic.score}</td>
                <td>{basic.alive ? "✅️" : "❌️"}</td>
            </tr>)}
            </tbody>
        </table>
        <div className="divider"></div>
        <GameControl />
    </div>;
}