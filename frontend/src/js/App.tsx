import React, {createContext, useState} from "react";
import "../css/App.css";
import "../css/ui.css";
import Lobby from "./Lobby.tsx";

export const UIContext = createContext(null as any);
export default function App() {
    const [UI, setUI] = useState(<Lobby />);
    return <UIContext.Provider value={[UI, setUI]}>
        <div id="game"></div>
        <div id="ui">{UI}</div>
    </UIContext.Provider>;
}