import {GAME_MAX_WIDTH, getFoodColor, getSnakeColor} from "./config.ts";
import React from "react";

export interface Point {
    x: number;
    y: number;
}

export interface SnakeBasic {
    speed: number;
    angle: number;
    width: number;
    length: number;
    score: number;
    alive: boolean;
    human: boolean;
}
export interface Snake extends SnakeBasic {
    segments: Point[];
}
export interface GameSnake extends Snake {
    color: string;
}

export interface Food {
    pos: Point;
    width: number;
}
export interface GameFood extends Food {
    color: string
}

export default interface GameState {
    tick: number;
    width: number;
    height: number;
    maxTick: number;
    snakes: GameSnake[];
    foods: Map<number, GameFood>;
}

export enum TickPolicyType {
    DEFINITION, INCREMENTAL
}
export enum EventApplyResultType {
    SUCCESS,
    NO_BASELINE,
    STALE,
    GAP,
    INVALID
}
export interface EventApplyResult {
    type: EventApplyResultType;
    state?: GameState;
}
export abstract class GameEvent {
    readonly tickPolicy: TickPolicyType;
    readonly tick: number;
    // Implementation may modify state but only if EventApplyResultType.SUCCESS is returned.
    // In other words, modification must not leave state in an invalid state.
    protected abstract doApply(state: GameState | undefined): EventApplyResult;
    constructor(tickPolicy: TickPolicyType, tick: number) {
        this.tickPolicy = tickPolicy;
        this.tick = tick;
    }
    apply(state: GameState | undefined) {
        if (state && this.tick <= state.tick) {
            return {type: EventApplyResultType.STALE};
        }
        if (this.tickPolicy === TickPolicyType.INCREMENTAL) {
            if (!state) return {type: EventApplyResultType.NO_BASELINE};
            if (this.tick !== state.tick + 1) return {type: EventApplyResultType.GAP};
        }
        return this.doApply(state);
    }
}

export function asKey(pos: Point) {
    return pos.x + GAME_MAX_WIDTH * pos.y;
}
export function decodeKey(key: number): Point {
    return {
        x: key % GAME_MAX_WIDTH,
        y: Math.floor(key / GAME_MAX_WIDTH)
    };
}

export class DeltaEvent extends GameEvent {
    readonly snakeBasics: SnakeBasic[];
    readonly foodsAdded: Food[];
    readonly foodsRemoved: Point[];
    constructor(tick: number, snake_basics: SnakeBasic[], foods_added: Food[], foods_removed: Point[]) {
        super(TickPolicyType.INCREMENTAL, tick);
        this.snakeBasics = snake_basics;
        this.foodsAdded = foods_added;
        this.foodsRemoved = foods_removed;
    }
    protected doApply(state: GameState) {
        for (let player_id = 0; player_id < this.snakeBasics.length; ++player_id) {
            state.snakes[player_id] = {...state.snakes[player_id], ...this.snakeBasics[player_id]};
            const snake = state.snakes[player_id];
            if (!snake.alive) continue;
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
                    x: prev1.x + (prev1.x - prev2.x),
                    y: prev1.y + (prev1.y - prev2.y)
                });
            }
        }
        for (const food of this.foodsAdded) {
            state.foods.set(asKey(food.pos), {...food, color: getFoodColor(food.width)});
        }
        for (const pos of this.foodsRemoved) {
            state.foods.delete(asKey(pos));
        }
        state.tick = this.tick;
        return {type: EventApplyResultType.SUCCESS, state: state};
    }
}

export class SnapshotEvent extends GameEvent {
    readonly width: number;
    readonly height: number;
    readonly maxTick: number;
    readonly snakes: Snake[];
    readonly foods: Food[];
    constructor(tick: number, width: number, height: number, maxTick: number, snakes: Snake[], foods: Food[]) {
        super(TickPolicyType.DEFINITION, tick);
        this.width = width;
        this.height = height;
        this.maxTick = maxTick;
        this.snakes = snakes;
        this.foods = foods;
    }
    protected doApply() {
        const state = {
            tick: this.tick,
            width: this.width,
            height: this.height,
            maxTick: this.maxTick,
            snakes: this.snakes.map((snake, i) => ({
                ...snake,
                color: getSnakeColor(i)
            } as GameSnake)),
            foods: new Map<number, GameFood>()
        };
        for (const food of this.foods) {
            state.foods.set(asKey(food.pos), {...food, color: getFoodColor(food.width)});
        }
        return {type: EventApplyResultType.SUCCESS, state: state};
    }
}

export class TerminationEvent extends GameEvent {
    readonly snakeBasics: SnakeBasic[];
    constructor(tick: number, snakeBasics: SnakeBasic[]) {
        super(TickPolicyType.INCREMENTAL, tick);
        this.snakeBasics = snakeBasics;
    }
    protected doApply(state: GameState) {
        state.tick = this.tick;
        for (let player_id = 0; player_id < this.snakeBasics.length; ++player_id) {
            state.snakes[player_id] = {...state.snakes[player_id], ...this.snakeBasics[player_id]};
        }
        return {type: EventApplyResultType.SUCCESS, state: state};
    }
}

export interface EventHandler {
    bind(state: React.RefObject<GameState | undefined>): void;
    handle(event: GameEvent): EventApplyResultType;
}
export class TrivialEventHandler implements EventHandler {
    #state: React.RefObject<GameState | undefined> | undefined;
    bind(state: React.RefObject<GameState | undefined>) {
        this.#state = state;
    }
    handle(event: GameEvent) {
        const result = event.apply(this.#state!.current);
        this.#state!.current = result.state;
        return result.type;
    }
}