import React, {useCallback, useContext, useEffect, useState} from "react";
import {Packet, packet_manager, PacketManager} from "./packet.ts";
import {UIContext} from "./App.tsx";
import {LobbyRoom, username_of} from "./Lobby.tsx";
import GameOver from "./GameOver.tsx";
import Termination from "./Termination.tsx";
import parsePacket, {PacketType} from "./parse.ts";
import {getSnakeColor} from "./config.ts";
import "../css/Game.css";
import useGameDisplay, {ScreenFocus, ScreenFocusType} from "./GameDisplay.ts";

enum Mode {
    GAME, SPECTATE, TERMINATION
}

interface ScoreBoardItem {
    player_id: number;
    username: string;
    score: number;
    color: string;
}

export default function Game({room, first_packet}: {room: LobbyRoom, first_packet: Packet}) {
    const [, setUI] = useContext(UIContext);
    const {
        containerRef, hexCanvasRef, foodCanvasRef, snakeCanvasRef, mapCanvasRef,
        screenWidth, screenHeight, bgState, screenFocus, getFocus
    } = useGameDisplay<HTMLDivElement>({
        initFocus: new ScreenFocus(ScreenFocusType.SNAKE, packet_manager.player_id)
    });
    const [mode, setMode] = useState(Mode.GAME);
    const [scoreBoard, setScoreBoard] = useState<ScoreBoardItem[]>([]);

    const listener = useCallback((packet: Packet) => {
        const current_alive = bgState.current?.snakes[packet_manager.player_id].alive;
        const event = parsePacket(bgState.current, packet);
        if (!event) return;
        bgState.current = event.apply(bgState.current);
        const state = bgState.current;
        if (event.type === PacketType.TERMINATION) {
            setMode(Mode.TERMINATION);
            setUI(<Termination room={room} basics={state!.snakes} />);
        } else {
            const new_alive = state?.snakes[packet_manager.player_id].alive;
            if (current_alive === true && new_alive === false) {
                setMode(Mode.SPECTATE);
                setUI(<GameOver score={state!.snakes[packet_manager.player_id].score} />);
            }
        }
        if (state) {
            setScoreBoard(state.snakes
                .map(((snake, player_id) => ({
                    player_id: player_id,
                    username: username_of(room, player_id),
                    score: snake.score,
                    color: getSnakeColor(player_id)
                } as ScoreBoardItem)))
                .filter(snake => state.snakes[snake.player_id].alive)
                .sort((a, b) => b.score - a.score)
            );
        } else {
            setScoreBoard([]);
        }
    }, [])
    useEffect(() => {
        listener(first_packet);
        packet_manager.add_listener(listener);
        return () => packet_manager.remove_listener(listener);
    }, []);

    const onGamePointerMove = (e: React.PointerEvent) => {
        if (e.buttons !== 1) return;

        const x = e.nativeEvent.offsetX;
        const y = e.nativeEvent.offsetY;
        
        const centerX = screenWidth / 2;
        const centerY = screenHeight / 2;
        
        const angle = Math.atan2(y - centerY, x - centerX);
        
        const packet = new Uint8Array(PacketManager.align(8));
        const view = new DataView(packet.buffer);
        view.setUint8(0, 0); // snapshot_requested = false
        view.setFloat32(4, angle, true);
        packet_manager.send(packet);
    };
    const onSpectatePointerMove = (e: React.PointerEvent) => {
        if (e.buttons !== 1) return;
        const focus = getFocus();
        if (!focus) return;
        screenFocus.current = new ScreenFocus(ScreenFocusType.POINT, {
            x: focus.x - e.movementX,
            y: focus.y - e.movementY
        });
    };
    const onScoreBoardItemClick =
        (e: React.MouseEvent<HTMLDivElement>) => {
            screenFocus.current = new ScreenFocus(ScreenFocusType.SNAKE, +e.currentTarget.dataset["player-id"]!);
        };
    const onMapClick = (e: React.MouseEvent) => {
        if (!bgState.current) return;
        const rect = mapCanvasRef.current!.getBoundingClientRect();
        const clickX = e.clientX - rect.left;
        const clickY = e.clientY - rect.top;
        screenFocus.current = new ScreenFocus(ScreenFocusType.POINT, {
            x: (clickX / mapCanvasRef.current!.width) * bgState.current.width,
            y: (clickY / mapCanvasRef.current!.height) * bgState.current.height
        });
    };

    return <div ref={containerRef} className="game"
                onPointerMove={[onGamePointerMove, onSpectatePointerMove, onSpectatePointerMove][mode]}>
        <canvas ref={hexCanvasRef} className="hex" height={screenHeight} width={screenWidth}></canvas>
        <canvas ref={foodCanvasRef} className="food" height={screenHeight} width={screenWidth}></canvas>
        <canvas ref={snakeCanvasRef} className="snake" height={screenHeight} width={screenWidth}></canvas>
        <canvas ref={mapCanvasRef} className="map" height={screenHeight / 8} width={screenWidth / 8}
                onClick={(mode === Mode.SPECTATE || mode === Mode.TERMINATION) ? onMapClick : undefined}></canvas>
        <table className="scoreboard">
            <tbody>{scoreBoard.map((item, idx) => {
                return <tr key={idx} data-player-id={item.player_id}
                           style={{color: item.color, cursor: mode === Mode.SPECTATE ? "pointer" : "auto"}}
                           onClick={mode === Mode.SPECTATE ? onScoreBoardItemClick : undefined}>
                    <td>{item.username}</td>
                    <td>{item.score}</td>
                </tr>;
            })}</tbody>
        </table>
    </div>;
}