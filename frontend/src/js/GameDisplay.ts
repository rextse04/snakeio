import GameState, {Point} from "./engine.ts";
import {RefObject, useCallback, useEffect, useRef, useState} from "react";
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

export default function useGameDisplay<
    ContainerT extends HTMLElement,
    GameStateT extends GameState = GameState
>({
    initGameState,
    initFocus,
    initRenderState,
    reconcile = (gameState, renderState) => {
        renderState.current = gameState.current;
    }
}: {
    initGameState?: GameStateT,
    initFocus?: ScreenFocus,
    initRenderState?: GameState,
    reconcile?: (
        gameState: RefObject<GameStateT | undefined>,
        renderState: RefObject<GameState | undefined>,
        gameGap: number, renderGap: number
    ) => void
}) {
    const containerRef = useRef<ContainerT>(null);
    const hexCanvasRef = useRef<HTMLCanvasElement>(null);
    const foodCanvasRef = useRef<HTMLCanvasElement>(null);
    const snakeCanvasRef = useRef<HTMLCanvasElement>(null);
    const mapCanvasRef = useRef<HTMLCanvasElement>(null);

    const [screenWidth, setScreenWidth] = useState(0);
    const [screenHeight, setScreenHeight] = useState(0);
    const gameState = useRef<GameStateT>(initGameState);
    const screenFocus = useRef(initFocus);
    const realFocus = useRef<Point>(undefined);

    const renderState = useRef<GameState>(initRenderState);
    const gameLastTick = useRef(-1);
    const gameLastTickTime = useRef<DOMHighResTimeStamp>(0);
    const renderLastTick = useRef(-1);
    const renderLastTickTime = useRef<DOMHighResTimeStamp>(0);

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
                return renderState.current?.snakes[screenFocus.current.asSnakeIndex()].segments[0];
            }
            case ScreenFocusType.POINT: {
                return screenFocus.current.asPoint();
            }
        }
    }, []);
    useEffect(() => {
        let animationFrameId: number;
        const loop = () => {
            const now = performance.now();
            const gameTick = gameState.current?.tick;
            if (gameTick && gameTick > gameLastTick.current) {
                gameLastTick.current = gameTick;
                gameLastTickTime.current = now;
            }
            reconcile(gameState, renderState, now - gameLastTickTime.current, now - renderLastTickTime.current);
            const renderTick = renderState.current?.tick;
            if (renderTick && renderTick > renderLastTick.current) {
                renderLastTick.current = renderTick;
                renderLastTickTime.current = now;
            }
            if (renderState.current) {
                const focus = getFocus();
                if (focus) realFocus.current = focus;
                render({
                    width: screenWidth,
                    height: screenHeight,
                    hex: hexCanvasRef.current,
                    food: foodCanvasRef.current,
                    snake: snakeCanvasRef.current,
                    map: mapCanvasRef.current
                }, renderState.current, realFocus.current);
            }
            animationFrameId = requestAnimationFrame(loop);
        };
        loop();
        return () => cancelAnimationFrame(animationFrameId);
    }, [screenWidth, screenHeight]);
    return {
        containerRef, hexCanvasRef, foodCanvasRef, snakeCanvasRef, mapCanvasRef,
        screenWidth, screenHeight, gameState, screenFocus, realFocus, renderState
    } as const;
}