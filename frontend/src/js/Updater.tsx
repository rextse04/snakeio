import {Update} from "@tauri-apps/plugin-updater";
import React, {useContext, useEffect, useState} from "react";
import {invoke} from "@tauri-apps/api/core";
import {UIContext} from "./App.tsx";
import {relaunch} from "@tauri-apps/plugin-process";

function decodeVersion(version: string) {
    return version.split(".").map(Number);
}
export default function Updater({update}: {update: Update}) {
    const [, setUI] = useContext(UIContext);
    const [downloaded, setDownloaded] = useState(0);
    const [total, setTotal] = useState(0);

    useEffect(() => {
        const current = decodeVersion(update.version);
        const latest = decodeVersion(update.currentVersion);
        const required = latest[0] > current[0];
        if (required) {
            if (!confirm("There is a new major version available. Do you want to update?")) {
                invoke("exit_app");
                return;
            }
        } else {
            if (!confirm("There is a new version available. Do you want to update?")) {
                setUI(undefined);
                return;
            }
        }
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
                    setUI(undefined);
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