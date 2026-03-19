import React, {useEffect, useRef} from "react";
import useGameDisplay, {ScreenFocus, ScreenFocusType} from "./GameDisplay.ts";
import GameState, {asKey, decodeKey, DeltaEvent, Food, GameFood, GameSnake, Point} from "./engine.ts";
import {getFoodColor, getSnakeColor} from "./config.ts";
import {angleDelta, clamp} from "./utils.ts";

interface BackgroundState extends GameState {
    targets: Point[];
}

function genFood(width: number, height: number): Food {
    return {
        pos: {x: Math.random() * width, y: Math.random() * height},
        width: 1 + Math.random() * 9
    };
}
function genTarget(width: number, height: number): Point {
    return {
        x: width/4 + Math.random() * width/2,
        y: height/4 + Math.random() * height/2
    };
}

export default function LobbyBackground() {
    const {
        containerRef, hexCanvasRef, foodCanvasRef, snakeCanvasRef,
        screenWidth, screenHeight, gameState, realFocus
    } = useGameDisplay<HTMLDivElement, BackgroundState>({
        initFocus: new ScreenFocus(ScreenFocusType.SNAKE, 0)
    });
    const pointerOffset = useRef<Point>(undefined);

    useEffect(() => {
        const onPointerMove = (e: PointerEvent) => {
            const rect = containerRef.current!.getBoundingClientRect();
            pointerOffset.current = {
                x: e.clientX - rect.left - rect.width / 2,
                y: e.clientY - rect.top - rect.height / 2
            };
        };
        document.addEventListener("pointermove", onPointerMove);
        return () => document.removeEventListener("pointermove", onPointerMove);
    }, []);

    useEffect(() => {
        const snakes = Array<GameSnake>(8);
        const width = screenWidth * 2;
        const height = screenHeight * 2;
        for (let i = 0; i < snakes.length; ++i) {
            const snake: GameSnake = snakes[i] = {
                speed: 2 + Math.random(),
                angle: -Math.PI + Math.random() * (2*Math.PI),
                width: 8 + Math.random() * 8,
                length: Math.round(Math.random() * 128),
                score: -1,
                alive: true,
                human: false,
                segments: [{x: Math.random() * width, y: Math.random() * height}],
                color: getSnakeColor(i)
            };
            for (let j = 1; j < snakes[i].length; ++j) {
                snake.segments[j] = {
                    x: snake.segments[j-1].x + Math.cos(snake.angle) * snake.width * 0.25,
                    y: snake.segments[j-1].y + Math.sin(snake.angle) * snake.width * 0.25
                };
            }
        }
        const foods = new Map<number, GameFood>();
        for (let i = 0; i < width * height / (64*64); ++i) {
            const food = genFood(width, height);
            foods.set(asKey(width, food.pos), {...food, color: getFoodColor(food.width)});
        }
        const targets = Array<Point>(snakes.length);
        for (let i = 0; i < snakes.length; ++i) {
            targets[i] = genTarget(width, height);
        }
        gameState.current = {
            tick: 0,
            width: width,
            height: height,
            maxTick: Infinity,
            snakes,
            foods,
            targets
        };
    }, [screenWidth, screenHeight]);

    useEffect(() => {
        const id = setInterval(() => {
            if (!gameState.current) return;
            const tick = gameState.current.tick;
            const snakes = gameState.current.snakes;
            const e = 1e-3;
            for (let i = 0; i < snakes.length; ++i) {
                const snake = snakes[i];
                const head = snake.segments[0];
                const target = gameState.current.targets[i];
                let dx = target.x - head.x;
                let dy = target.y - head.y;
                if (tick % 512 === 0 || dx*dx + dy*dy < snake.width*snake.width) {
                    const new_target = genTarget(gameState.current.width, gameState.current.height);
                    target.x = new_target.x;
                    target.y = new_target.y;
                }
                dx = target.x - head.x;
                dy = target.y - head.y;
                const d = Math.sqrt(dx*dx + dy*dy) + e;
                dx /= d; dy /= d;
                let mc = 500, mdx = 0, mdy = 0;
                if (i !== 0 && pointerOffset.current) {
                    const focus = realFocus.current;
                    if (!focus) continue;
                    mdx = focus.x + pointerOffset.current.x - head.x;
                    mdy = focus.y + pointerOffset.current.y - head.y;
                    const m2 = mdx*mdx + mdy*mdy + e;
                    mdx /= m2; mdy /= m2;
                    if (m2 < 100**2) mc *= -1;
                }
                const new_angle = Math.atan2(dy + mdy * mc, dx + mdx * mc);
                snake.angle += clamp(angleDelta(new_angle, snake.angle), -Math.PI / 100, Math.PI / 100);
            }
            const foodsAdded: Food[] = [];
            const foodsRemoved: Point[] = [];
            if (tick % 8 === 0) {
                foodsAdded.push(genFood(gameState.current.width, gameState.current.height));
                const foods = gameState.current.foods;
                const removed = foods.keys().next().value;
                if (removed) foodsRemoved.push(decodeKey(gameState.current.width, removed));
            }
            new DeltaEvent(tick + 1, snakes, foodsAdded, foodsRemoved).apply(gameState.current);
        }, 20);
        return () => clearInterval(id);
    }, []);

    return <div ref={containerRef} className="game">
        <canvas ref={hexCanvasRef} className="hex" height={screenHeight} width={screenWidth}></canvas>
        <canvas ref={foodCanvasRef} className="food" height={screenHeight} width={screenWidth}></canvas>
        <canvas ref={snakeCanvasRef} className="snake" height={screenHeight} width={screenWidth}></canvas>
    </div>;
}