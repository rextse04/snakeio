import React, {useContext, useEffect, useState} from "react";
import {getName, getVersion} from "@tauri-apps/api/app";
import {check} from "@tauri-apps/plugin-updater";
import {UIContext} from "./App.tsx";
import Lobby from "./Lobby.tsx";
import {UpdateDialog} from "./Updater.tsx";

export default function About() {
    const [, setUI] = useContext(UIContext);
    const [name, setName] = useState("");
    const [version, setVersion] = useState("");
    useEffect(() => {
        getName().then(setName);
        getVersion().then(setVersion);
    }, []);
    const [checkingUpdate, setCheckingUpdate] = useState(false);

    const onUpdate = () => {
        setCheckingUpdate(true);
        check()
            .then(update => {
                if (update) setUI(<UpdateDialog update={update} />)
                else alert("You are already up to date!");
            })
            .catch(error => alert(`Failed to check for updates. Reason: ${error}.`))
            .finally(() => setCheckingUpdate(false));
    };
    const onLobby = () => setUI(<Lobby />);

    return <div>
        <h1>{name}</h1>
        <div>A true multiplayer clone of <i>slither.io</i>.</div>
        <table>
            <tbody>
            <tr>
                <th scope="row">Version</th>
                <td>{version}</td>
            </tr>
            <tr>
                <th scope="row">Author</th>
                <td>Rex Tse</td>
            </tr>
            </tbody>
        </table>
        <div className="divider"></div>
        <div className="control-group">
            <button onClick={onUpdate} disabled={checkingUpdate}>Check for updates</button>
            <button onClick={onLobby}>Return to lobby</button>
        </div>
    </div>
}