import {Update} from "@tauri-apps/plugin-updater";
import React, {useContext, useEffect, useState} from "react";
import {invoke} from "@tauri-apps/api/core";
import {UIContext} from "./App.tsx";
import {relaunch} from "@tauri-apps/plugin-process";
import Lobby from "./Lobby.tsx";

function decodeVersion(version: string) {
    return version.split(".").map(Number);
}
export function UpdateDialog({update}: {update: Update}) {
    const [, setUI] = useContext(UIContext);

    const current = decodeVersion(update.currentVersion);
    const latest = decodeVersion(update.version);
    const required = latest[0] > current[0];
    return <div>
        <h1>Update available</h1>
        <table>
            <tbody>
            <tr>
                <th scope="row">Current version</th>
                <td>{update.currentVersion}</td>
            </tr>
            <tr>
                <th scope="row">Latest version</th>
                <td>{update.version}</td>
            </tr>
            <tr>
                <th scope="row">Required</th>
                <td>{required ? "Yes" : "No"}</td>
            </tr>
            {update.date && <tr>
                <th scope="row">Date</th>
                <td>{update.date}</td>
            </tr>}
            {update.body && <tr>
                <th scope="row">Release notes</th>
                <td>{update.body}</td>
            </tr>}
            </tbody>
        </table>
        <div className="divider"></div>
        <div className="control-group">
            <button onClick={() => setUI(<Updater update={update} required={required} />)}>Update now</button>
            {required ? undefined : <button onClick={() => setUI(<Lobby />)}>Return to lobby</button>}
        </div>
    </div>;
}
export function Updater({update, required}: {update: Update, required: boolean}) {
    const [, setUI] = useContext(UIContext);
    const [downloaded, setDownloaded] = useState(0);
    const [total, setTotal] = useState(0);

    useEffect(() => {
        update.downloadAndInstall(event => {
            switch (event.event) {
                case "Started": {
                    setTotal(event.data.contentLength || 0);
                    break;
                }
                case "Progress": {
                    setDownloaded(downloaded => downloaded + event.data.chunkLength);
                    break;
                }
            }
        }).then(
            () => {
                if (!required && !confirm("Update complete. Would you like to relaunch the app now?")) {
                    setUI(<Lobby />);
                    return;
                }
                relaunch();
            },
            error => {
                console.error(error);
                alert("Failed to update. Please try again later.");
                if (required) invoke("exit_app");
                else setUI(undefined);
            }
        );
    }, []);

    return <div className="updater">
        <h1>Updating...</h1>
        <div>
            <progress value={downloaded} max={total}></progress>
        </div>
    </div>;
}