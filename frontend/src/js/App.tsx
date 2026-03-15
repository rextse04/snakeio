import React, {createContext, JSX, useState} from "react";
import "../css/App.css";
import "../css/ui.css";
import Lobby from "./Lobby.tsx";

function useElement(element: JSX.Element | undefined) {
    return useState(element);
}

export const GameContext =
    createContext(null as unknown as ReturnType<typeof useElement>);
export const UIContext =
    createContext(null as unknown as ReturnType<typeof useElement>);
export default function App() {
    const [game, setGame] = useElement(undefined);
    const [UI, setUI] = useElement(<Lobby />);
    return <GameContext.Provider value={[game, setGame]}>
    <UIContext.Provider value={[UI, setUI]}>
        <div id="game">{game}</div>
        {UI && <div id="ui">{UI}</div>}
    </UIContext.Provider>
    </GameContext.Provider>;
}