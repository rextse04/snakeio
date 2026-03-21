import React from "react";
import {useContext} from "react";
import {GameContext, UIContext} from "./App.tsx";
import Lobby from "./Lobby.tsx";

export default function GameControl() {
    const [, setGame] = useContext(GameContext);
    const [, setUI,, setHidden] = useContext(UIContext);
    return <div className="control-group">
        <button onClick={() => setHidden(true)}>Spectate</button>
        <button onClick={() => {
            setGame(undefined);
            setUI(<Lobby />);
        }}>Return to lobby</button>
    </div>;
}