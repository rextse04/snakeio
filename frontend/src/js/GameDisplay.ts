import GameState, {Point} from "./engine.ts";
import {useCallback, useEffect, useRef, useState} from "react";
import render from "./render.ts";

export enum ScreenFocusType {
    SNAKE, POINT
}
export class ScreenFocus {
    readonly #type: ScreenFocusType;
    readonly #value: number | Point;
    constructor(type: ScreenFocusType, value: number | Point) {
        this.#type = type;
        this.#value = value;
    }
    get type() {
        return this.#type;
    }
    asSnakeIndex() {
        return this.#value as number;
    }
    asPoint() {
        return this.#value as Point;
    }
}

export default function useGameDisplay
    <ContainerT extends HTMLElement, GameStateT extends GameState = GameState>(
    {initGameState, initFocus}:
    {initGameState?: GameStateT, initFocus?: ScreenFocus}) {
    const containerRef = useRef<ContainerT>(null);
    const hexCanvasRef = useRef<HTMLCanvasElement>(null);
    const foodCanvasRef = useRef<HTMLCanvasElement>(null);
    const snakeCanvasRef = useRef<HTMLCanvasElement>(null);
    const mapCanvasRef = useRef<HTMLCanvasElement>(null);

    const [screenWidth, setScreenWidth] = useState(0);
    const [screenHeight, setScreenHeight] = useState(0);
    const gameState = useRef<GameStateT>(initGameState);
    const screenFocus = useRef(initFocus);

    useEffect(() => {
        if (!containerRef.current) {
            throw new Error("No container element bound to useGameDisplay.");
        }
        setScreenWidth(containerRef.current.clientWidth);
        setScreenHeight(containerRef.current.clientHeight);
        const observer = new ResizeObserver(entries => {
            if (entries.length !== 1) return;
            const entry = entries[0];
            setScreenWidth(entry.contentRect.width);
            setScreenHeight(entry.contentRect.height);
        });
        observer.observe(containerRef.current);
        return () => observer.disconnect();
    }, []);

    const getFocus = useCallback(() => {
        if (!screenFocus.current) return undefined;
        switch (screenFocus.current.type) {
            case ScreenFocusType.SNAKE: {
                return gameState.current?.snakes[screenFocus.current.asSnakeIndex()].segments[0];
            }
            case ScreenFocusType.POINT: {
                return screenFocus.current.asPoint();
            }
        }
    }, []);
    useEffect(() => {
        let animationFrameId: number;
        const loop = () => {
            if (gameState.current) {
                render({
                    width: screenWidth,
                    height: screenHeight,
                    hex: hexCanvasRef.current,
                    food: foodCanvasRef.current,
                    snake: snakeCanvasRef.current,
                    map: mapCanvasRef.current
                }, gameState.current, getFocus());
            }
            animationFrameId = requestAnimationFrame(loop);
        };
        loop();
        return () => cancelAnimationFrame(animationFrameId);
    }, [screenWidth, screenHeight]);
    return {
        containerRef, hexCanvasRef, foodCanvasRef, snakeCanvasRef, mapCanvasRef,
        screenWidth, screenHeight, bgState: gameState, screenFocus, getFocus
    } as const;
}