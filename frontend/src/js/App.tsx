import React, {createContext, JSX, SetStateAction, useEffect, useState} from "react";
import Lobby from "./Lobby.tsx";
import {invoke} from "@tauri-apps/api/core";
import {check} from "@tauri-apps/plugin-updater";
import Updater from "./Updater.tsx";

import "@fortawesome/fontawesome-free/css/all.min.css"
import "../css/App.css";
import "../css/game.css";
import "../css/ui.css";

type AppConfig = {
    control_server_addr: string;
    debug_drop_outgoing_enabled: boolean;
    debug_drop_outgoing_chance: number;
    debug_drop_incoming_enabled: boolean;
    debug_drop_incoming_chance: number;
};
export const app_config = await invoke<AppConfig>("get_config");

function useGame(element: JSX.Element | undefined) {
    return useState(element);
}
function useUI(element: JSX.Element | undefined) {
    const [UI, _setUI] = useState(element);
    const [hidden, setHidden] = useState(false);
    const setUI = (nextState: SetStateAction<typeof element>) => {
        _setUI(nextState);
        setHidden(false);
    };
    return [UI, setUI, hidden, setHidden] as const;
}

export const GameContext =
    createContext(null as unknown as ReturnType<typeof useGame>);
export const UIContext =
    createContext(null as unknown as ReturnType<typeof useUI>);
export default function App() {
    const [game, setGame] = useGame(undefined);
    const [UI, setUI, hidden, setHidden] = useUI(<Lobby />);

    useEffect(() => {
        check().then(update => setUI(update ? <Updater update={update} /> : undefined));
    }, []);

    useEffect(() => {
        const onKeyDown = (e: KeyboardEvent) => {
            if (e.key === "Escape") {
                setHidden(false);
            }
        };
        addEventListener("keydown", onKeyDown);
        return () => removeEventListener("keydown", onKeyDown);
    }, []);

    return <GameContext.Provider value={[game, setGame]}>
    <UIContext.Provider value={[UI, setUI, hidden, setHidden]}>
        <div id="game">{game}</div>
        {UI && <div id="ui" style={{visibility: hidden ? "hidden" : "visible"}}>{UI}</div>}
    </UIContext.Provider>
    </GameContext.Provider>;
}