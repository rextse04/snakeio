import React, {RefObject, useCallback, useContext, useEffect, useMemo, useRef, useState} from "react";
import {Packet, packet_manager, PacketManager} from "./packet.ts";
import {UIContext} from "./App.tsx";
import {LobbyRoom, usernameOf} from "./Lobby.tsx";
import GameOver from "./GameOver.tsx";
import Termination from "./Termination.tsx";
import parsePacket, {PacketType} from "./parse.ts";
import GameConfig, {getSnakeColor} from "./config.ts";
import useGameDisplay, {ScreenFocus, ScreenFocusType} from "./GameDisplay.ts";
import EventBuffer from "./EventBuffer.ts";
import {formatMs, useStateRef} from "./utils.ts";
import GameState, {DeltaEvent, isSnakeAlive} from "./engine.ts";
import "../css/Game.css";

enum Mode {
    GAME, SPECTATE, TERMINATION
}

interface ScoreBoardItem {
    player_id: number;
    username: string;
    score: number;
    color: string;
}

export default function Game({config, room, first_packet}:
                             {config: GameConfig, room: LobbyRoom, first_packet: Packet}) {
    const [, setUI] = useContext(UIContext);
    const [mode, modeRef, setMode] = useStateRef(Mode.GAME);
    const boostRef = useRef(false);
    const reconcile =
        useCallback((
            gameState: RefObject<GameState | undefined>, renderState: RefObject<GameState | undefined>,
            gameGap: number, renderGap: number) => {
            if (!gameState.current) return;
            if (modeRef.current === Mode.TERMINATION || !renderState.current || gameGap <= config.game_max_tick) {
                renderState.current = gameState.current;
                return;
            }
            if (gameState.current.tick >= renderState.current.tick) {
                renderState.current = structuredClone(gameState.current);
            } else {
                if (renderGap <= config.game_max_tick) return;
            }

            const state = renderState.current;
            renderState.current = new DeltaEvent(state.tick + 1, state.snakes, [], []).apply(state).state;
        }, []);
    const {
        containerRef, hexCanvasRef, foodCanvasRef, snakeCanvasRef, mapCanvasRef,
        screenWidth, screenHeight, gameState, screenFocus, realFocus, renderState
    } = useGameDisplay<HTMLDivElement>({
        initFocus: new ScreenFocus(ScreenFocusType.SNAKE, packet_manager.player_id),
        reconcile
    });
    const angle = useRef(NaN);
    const [scoreBoard, setScoreBoard] = useState<ScoreBoardItem[]>([]);

    const [, panicRef, setPanic] = useStateRef(false);
    const sendPacket = useCallback((snapshotRequested = false) => {
        if (modeRef.current === Mode.TERMINATION) return;
        const packet = new Uint8Array(PacketManager.align(8));
        const view = new DataView(packet.buffer);
        view.setUint8(0, +(snapshotRequested || panicRef.current));
        view.setUint8(1, +boostRef.current);
        view.setFloat32(4, angle.current, true);
        packet_manager.send(packet);
    }, []);
    const eventHandler = useMemo(() => {
        const handler = new EventBuffer({
            timeoutPerPacket: config.tick_rate_ms * 2,
            panic: () => {
                sendPacket(true);
                setPanic(true);
            },
            resolve: () => {
                setPanic(false)
            },
            onStateChange: state => {
                if (modeRef.current === Mode.GAME && state && !isSnakeAlive(state.snakes[packet_manager.player_id])) {
                    setMode(Mode.SPECTATE);
                    setUI(<GameOver score={state!.snakes[packet_manager.player_id].score} />);
                }
                if (state) {
                    setScoreBoard(state.snakes
                        .map(((snake, player_id) => ({
                            player_id: player_id,
                            username: usernameOf(room, player_id),
                            score: snake.score,
                            color: getSnakeColor(player_id)
                        } as ScoreBoardItem)))
                        .filter(snake => isSnakeAlive(state.snakes[snake.player_id]))
                        .sort((a, b) => b.score - a.score)
                    );
                } else {
                    setScoreBoard([]);
                }
            }
        });
        handler.bind(gameState);
        return handler;
    }, []);
    useEffect(() => {
        const id = setInterval(() => {
            if (modeRef.current !== Mode.GAME) {
                clearInterval(id);
                return;
            }
            sendPacket();
        }, config.tick_rate_ms / 2);
        return () => clearInterval(id);
    }, []);

    const listener = useCallback((packet: Packet) => {
        const players = gameState.current?.snakes.length;
        const {type, event} = parsePacket(packet, players);
        if (type === PacketType.TERMINATION) {
            setMode(Mode.TERMINATION);
            modeRef.current = Mode.TERMINATION;
            setUI(<Termination room={room} snakes={event.snakeBasics} />);
        }
        eventHandler.handle(event);
    }, [])
    useEffect(() => {
        listener(first_packet);
        packet_manager.add_listener(listener);
        return () => packet_manager.remove_listener(listener);
    }, []);

    const onGamePointerMove = (e: React.PointerEvent) => {
        const x = e.nativeEvent.offsetX;
        const y = e.nativeEvent.offsetY;
        const centerX = screenWidth / 2;
        const centerY = screenHeight / 2;
        angle.current = Math.atan2(y - centerY, x - centerX);
    };
    const onSpectatePointerMove = (e: React.PointerEvent) => {
        if (e.buttons !== 1) return;
        const focus = realFocus.current
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
        if (!gameState.current) return;
        const rect = mapCanvasRef.current!.getBoundingClientRect();
        const clickX = e.clientX - rect.left;
        const clickY = e.clientY - rect.top;
        screenFocus.current = new ScreenFocus(ScreenFocusType.POINT, {
            x: (clickX / mapCanvasRef.current!.width) * gameState.current.width,
            y: (clickY / mapCanvasRef.current!.height) * gameState.current.height
        });
    };

    const onKeyDown = (e: KeyboardEvent) => {
        if (e.key !== " ") return;
        boostRef.current = true;
    };
    const onKeyUp = (e: KeyboardEvent) => {
        if (e.key !== " ") return;
        boostRef.current = false;
    };
    useEffect(() => {
        document.addEventListener("keydown", onKeyDown);
        document.addEventListener("keyup", onKeyUp);
        return () => {
            document.removeEventListener("keydown", onKeyDown);
            document.removeEventListener("keyup", onKeyUp);
        };
    }, []);

    return <div ref={containerRef} className="game"
                onPointerMove={[onGamePointerMove, onSpectatePointerMove, onSpectatePointerMove][mode]}>
        <canvas ref={hexCanvasRef} className="hex" height={screenHeight} width={screenWidth}></canvas>
        <canvas ref={foodCanvasRef} className="food" height={screenHeight} width={screenWidth}></canvas>
        <canvas ref={snakeCanvasRef} className="snake" height={screenHeight} width={screenWidth}></canvas>
        <canvas ref={mapCanvasRef} className="map" height={screenHeight / 8} width={screenWidth / 8}
                onClick={(mode === Mode.SPECTATE || mode === Mode.TERMINATION) ? onMapClick : undefined}></canvas>
        {renderState.current && <div className="timer">
            {formatMs(
                (renderState.current.maxTick - renderState.current.tick + +(mode === Mode.TERMINATION)) *
                config.tick_rate_ms
            )}
        </div>}
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