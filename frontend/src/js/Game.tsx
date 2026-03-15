import React, {useCallback, useContext, useEffect, useRef, useState} from "react";
import {packet_manager, PacketManager} from "./packet.ts";
import {UIContext} from "./App.tsx";
import Lobby, {all_players, LobbyRoom} from "./Lobby.tsx";
import "../css/Game.css";

interface Point {
    x: number;
    y: number;
}

interface SnakeBasic {
    speed: number;
    angle: number;
    width: number;
    length: number;
    score: number;
    alive: boolean;
    human: boolean;
}
interface Snake extends SnakeBasic {
    segments: Point[];
    color: string;
}

interface Food {
    pos: Point;
    width: number;
    color: string;
}

const SNAKE_COLORS = [
    "#C0392B",
    "#E74C3C",
    "#9B59B6",
    "#8E44AD",
    "#2980B9",
    "#3498DB",
    "#17A589",
    "#138D75",
    "#229954",
    "#28B463",
    "#D4AC0D",
    "#D68910",
    "#CA6F1E",
    "#BA4A00"
];
const FOOD_COLORS = [
    "#ff0000",
    "#00ff00",
    "#0000ff",
    "#ffff00",
    "#ff00ff",
    "#00ffff",
    "#ffa500"
];

const GAME_MAX_WIDTH = 32768, GAME_MAX_HEIGHT = 16784;
const SNAKE_BASIC_SIZE = 24;
const SEGMENT_SIZE = 8;
const FOOD_SIZE = 12;

function asKey(x: number, y: number) {
    return x + GAME_MAX_WIDTH * y;
}
function getSnakeColor(index: number) {
    return SNAKE_COLORS[index % SNAKE_COLORS.length];
}
function getFoodColor(radius: number) {
    return FOOD_COLORS[Math.floor(radius) % FOOD_COLORS.length];
}
function parseSnakeBasic(view: DataView, offset: number) {
    const speed = view.getFloat32(offset, true);
    const angle = view.getFloat32(offset + 4, true);
    const radius = view.getFloat32(offset + 8, true);
    const length = view.getUint32(offset + 12, true);
    const score = view.getUint32(offset + 16, true);
    const alive = view.getUint8(offset + 20) !== 0;
    const human = view.getUint8(offset + 21) !== 0;
    return {
        basic: {speed, angle, width: radius, length, score, alive, human} as SnakeBasic,
        nextOffset: offset + SNAKE_BASIC_SIZE
    };
}
function parseFood(view: DataView, offset: number) {
    const x = view.getFloat32(offset, true);
    const y = view.getFloat32(offset + 4, true);
    const radius = view.getFloat32(offset + 8, true);
    return {
        food: {pos: {x, y}, width: radius, color: getFoodColor(radius)} as Food,
        nextOffset: offset + FOOD_SIZE
    };
}
export default function Game({room}: {room: LobbyRoom}) {
    const [, setUI] = useContext(UIContext);
    const snakeCanvasRef = useRef<HTMLCanvasElement>(null);
    const foodCanvasRef = useRef<HTMLCanvasElement>(null);
    const hexCanvasRef = useRef<HTMLCanvasElement>(null);
    const [screenWidth, setScreenWidth] = useState(document.documentElement.clientWidth);
    const [screenHeight, setScreenHeight] = useState(document.documentElement.clientHeight);

    const worldSize = useRef<Point>({x: 0, y: 0});
    const snakes = useRef<Snake[]>([]);
    const foods = useRef(new Map<number, Food>());
    const maxTick = useRef(0);

    useEffect(() => {
        const listener = (tick: number, data: Uint8Array) => {
            const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
            const type = view.getUint32(0, true);
            switch(type) {
                case 0: { // delta
                    let offset = 4;
                    for (let i = 0; i < all_players(room); i++) {
                        const {basic, nextOffset} = parseSnakeBasic(view, offset);
                        offset = nextOffset;
                        if (!(snakes.current[i]?.alive)) continue;
                        snakes.current[i] = {...snakes.current[i], ...basic};
                        const snake = snakes.current[i];
                        // Move snake
                        snake.segments.unshift({
                            x: snake.segments[0].x + snake.speed * Math.cos(snake.angle),
                            y: snake.segments[0].y + snake.speed * Math.sin(snake.angle)
                        });
                        snake.segments.pop();
                        // Extend snake if grown
                        while (snake.segments.length < snake.length) {
                            const prev1 = snake.segments[snake.segments.length - 1];
                            const prev2 = snake.segments[snake.segments.length - 2];
                            snake.segments.push({
                                x: prev1.x - prev2.x,
                                y: prev1.y - prev2.y
                            });
                        }
                    }

                    const foodsAddedSize = view.getUint32(offset, true);
                    offset += 4;
                    for (let i = 0; i < foodsAddedSize; i++) {
                        const {food, nextOffset} = parseFood(view, offset);
                        foods.current.set(asKey(food.pos.x, food.pos.y), food);
                        offset = nextOffset;
                    }

                    const foodsRemovedSize = view.getUint32(offset, true);
                    offset += 4;
                    for (let i = 0; i < foodsRemovedSize; i++) {
                        const rx = view.getFloat32(offset, true);
                        const ry = view.getFloat32(offset + 4, true);
                        foods.current.delete(asKey(rx, ry));
                        offset += 8;
                    }
                    break;
                }
                case 1: { // snapshot
                    worldSize.current = {
                        x: view.getFloat32(4, true),
                        y: view.getFloat32(8, true)
                    };
                    maxTick.current = view.getUint32(12, true);

                    let offset = 16;

                    for (let i = 0; i < all_players(room); i++) {
                        const {basic, nextOffset} = parseSnakeBasic(view, offset);
                        offset = nextOffset;
                        const segments: Point[] = [];
                        for (let j = 0; j < basic.length; j++) {
                            segments[j] = {
                                x: view.getFloat32(offset, true),
                                y: view.getFloat32(offset + 4, true)
                            };
                            offset += SEGMENT_SIZE;
                        }
                        snakes.current[i] = {...basic, segments, color: getSnakeColor(i)};
                    }

                    const foodsSize = view.getUint32(offset, true);
                    offset += 4;
                    foods.current.clear();
                    for (let i = 0; i < foodsSize; i++) {
                        const {food, nextOffset} = parseFood(view, offset);
                        foods.current.set(asKey(food.pos.x, food.pos.y), food);
                        offset = nextOffset;
                    }
                    break;
                }
                case 3: { // termination
                    setUI(<Lobby />);
                    break;
                }
                default: {
                    console.error("Unknown packet type", type, data);
                    break;
                }
            }
        };
        packet_manager.add_listener(listener);
        const packet = new ArrayBuffer(PacketManager.align(8));
        new DataView(packet).setUint32(0, 1, true);
        packet_manager.send(new Uint8Array(packet));
        return () => packet_manager.remove_listener(listener);
    }, []);

    const draw = useCallback(() => {
        if (packet_manager.tick < 0) return;

        const snakeCtx = snakeCanvasRef.current!.getContext("2d")!;
        const foodCtx = foodCanvasRef.current!.getContext("2d")!;
        const hexCtx = hexCanvasRef.current!.getContext("2d")!;

        snakeCtx.clearRect(0, 0, screenWidth, screenHeight);
        foodCtx.clearRect(0, 0, screenWidth, screenHeight);
        hexCtx.clearRect(0, 0, screenWidth, screenHeight);

        const mySnake = snakes.current[packet_manager.player_id];
        const head = mySnake.segments[0];
        const offsetX = screenWidth / 2 - head.x;
        const offsetY = screenHeight / 2 - head.y;

        // Draw World Boundary
        hexCtx.strokeStyle = "white";
        hexCtx.lineWidth = 2;
        hexCtx.strokeRect(offsetX, offsetY, worldSize.current.x, worldSize.current.y);
        hexCtx.fillStyle = "#17202A";
        hexCtx.fillRect(offsetX, offsetY, worldSize.current.x, worldSize.current.y);

        // Draw Food
        foods.current.forEach(food => {
            const screenX = food.pos.x + offsetX;
            const screenY = food.pos.y + offsetY;

            foodCtx.globalAlpha = 0.5;
            foodCtx.fillStyle = food.color;
            foodCtx.beginPath();
            foodCtx.arc(screenX, screenY, food.width, 0, 2 * Math.PI);
            foodCtx.fill();

            foodCtx.globalAlpha = 1;
            foodCtx.beginPath();
            foodCtx.arc(screenX, screenY, food.width / 2, 0, 2 * Math.PI);
            foodCtx.fill();
        });

        // Draw Snakes
        snakes.current.forEach(snake => {
            if (!snake.alive) return;
            
            snakeCtx.fillStyle = snake.color;
            snake.segments.forEach((seg, i) => {
                const screenX = seg.x + offsetX;
                const screenY = seg.y + offsetY;

                snakeCtx.beginPath();
                snakeCtx.arc(screenX, screenY, snake.width, 0, 2 * Math.PI);
                snakeCtx.fill();
                
                if (i === 0) { // Head
                    // Draw eyes
                    snakeCtx.fillStyle = "white";
                    const eyeDist = snake.width * 0.7;
                    const eyeRadius = snake.width * 0.3;
                    
                    // Left eye
                    const leX = screenX + eyeDist * Math.cos(snake.angle - 0.5);
                    const leY = screenY + eyeDist * Math.sin(snake.angle - 0.5);
                    snakeCtx.beginPath();
                    snakeCtx.arc(leX, leY, eyeRadius, 0, 2 * Math.PI);
                    snakeCtx.fill();
                    
                    // Right eye
                    const reX = screenX + eyeDist * Math.cos(snake.angle + 0.5);
                    const reY = screenY + eyeDist * Math.sin(snake.angle + 0.5);
                    snakeCtx.beginPath();
                    snakeCtx.arc(reX, reY, eyeRadius, 0, 2 * Math.PI);
                    snakeCtx.fill();

                    // Pupils
                    snakeCtx.fillStyle = "black";
                    snakeCtx.beginPath();
                    snakeCtx.arc(
                        leX + Math.cos(snake.angle) * eyeRadius/2,
                        leY + Math.sin(snake.angle) * eyeRadius/2,
                        eyeRadius/2, 0, 2 * Math.PI);
                    snakeCtx.fill();
                    snakeCtx.beginPath();
                    snakeCtx.arc(
                        reX + Math.cos(snake.angle) * eyeRadius/2,
                        reY + Math.sin(snake.angle) * eyeRadius/2,
                        eyeRadius/2, 0, 2 * Math.PI);
                    snakeCtx.fill();

                    snakeCtx.fillStyle = snake.color;
                }
            });
        });

        // Draw Scores
        snakeCtx.fillStyle = "white";
        snakeCtx.font = "bold 18px Arial";
        const scoreboard = [];
        for (let i = 0; i < all_players(room); i++) {
            const snake = snakes.current[i];
            if (!snake.alive) continue;
            const username = (i < room.players.length)
                ? room.players[i].username
                : ("AI " + (i - room.players.length + 1));
            scoreboard.push({
                username: username,
                score: snake.score,
                color: snake.color
            });
        }
        scoreboard.sort((a, b) =>
            b.score - a.score);
        scoreboard.forEach((item, i) => {
            snakeCtx.fillStyle = item.color;
            snakeCtx.fillText(`${item.username}: ${item.score}`, 20, 30 + i * 20);
        });

        // Draw Map
        const mapSize = {x: screenWidth / 8, y: screenHeight / 8};
        const mapPos = {x: screenWidth - mapSize.x * 1.2, y: screenHeight - mapSize.y * 1.2};
        snakeCtx.globalAlpha = 0.5;
        snakeCtx.fillStyle = "white";
        snakeCtx.fillRect(mapPos.x, mapPos.y, mapSize.x, mapSize.y);
        snakeCtx.globalAlpha = 1.0;
        
        snakes.current.forEach((snake) => {
            if (!snake.alive || snake.segments.length === 0) return;
            const px = mapPos.x + (snake.segments[0].x / worldSize.current.x) * mapSize.x;
            const py = mapPos.y + (snake.segments[0].y / worldSize.current.y) * mapSize.y;
            snakeCtx.fillStyle = snake.color;
            snakeCtx.beginPath();
            snakeCtx.arc(px, py, 2, 0, 2 * Math.PI);
            snakeCtx.fill();
        });
    }, [screenWidth, screenHeight]);

    useEffect(() => {
        let animationFrameId: number;
        const loop = () => {
            draw();
            animationFrameId = requestAnimationFrame(loop);
        };
        loop();
        return () => cancelAnimationFrame(animationFrameId);
    }, [draw]);

    useEffect(() => {
        const handleResize = () => {
            setScreenWidth(document.documentElement.clientWidth);
            setScreenHeight(document.documentElement.clientHeight);
        };
        window.addEventListener("resize", handleResize);
        return () => window.removeEventListener("resize", handleResize);
    }, []);

    const onPointerMove = (e: React.PointerEvent) => {
        if (e.buttons !== 1) return;

        const rect = snakeCanvasRef.current!.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;
        
        const centerX = screenWidth / 2;
        const centerY = screenHeight / 2;
        
        const angle = Math.atan2(y - centerY, x - centerX);
        
        const packet = new Uint8Array(PacketManager.align(8));
        const view = new DataView(packet.buffer);
        view.setUint32(0, 0, true); // snapshot_requested = false
        view.setFloat32(4, angle, true);
        packet_manager.send(packet);
    };

    return <div className="game">
        <canvas ref={hexCanvasRef} className="hex" height={screenHeight} width={screenWidth}></canvas>
        <canvas ref={foodCanvasRef} className="food" height={screenHeight} width={screenWidth}></canvas>
        <canvas ref={snakeCanvasRef} className="snake" height={screenHeight} width={screenWidth}
                onPointerMove={onPointerMove}></canvas>
    </div>;
}