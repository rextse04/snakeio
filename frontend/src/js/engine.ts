import {GAME_MAX_WIDTH, getFoodColor, getSnakeColor} from "./config.ts";

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

export abstract class GameEvent {
    readonly tick: number;
    protected abstract doApply(state: GameState | undefined): GameState | undefined;
    constructor(tick: number) {
        this.tick = tick;
    }
    apply(state: GameState | undefined) {
        if (state && state.tick >= this.tick) return undefined;
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
        super(tick);
        this.snakeBasics = snake_basics;
        this.foodsAdded = foods_added;
        this.foodsRemoved = foods_removed;
    }
    protected doApply(state: GameState | undefined) {
        if (!state) return undefined;
        for (let player_id = 0; player_id < this.snakeBasics.length; ++player_id) {
            state.snakes[player_id] = {...state.snakes[player_id], ...this.snakeBasics[player_id]};
            const snake = state.snakes[player_id];
            if (!snake.alive) continue;
            // Move snake
            const head: Point[] = [];
            for (let dt = this.tick - state.tick; dt > 0; dt--) {
                head.push({
                    x: snake.segments[0].x + snake.speed * Math.cos(snake.angle) * dt,
                    y: snake.segments[0].y + snake.speed * Math.sin(snake.angle) * dt
                });
            }
            snake.segments = [...head, ...snake.segments.slice(0, -head.length)];
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
        return state;
    }
}

export class SnapshotEvent extends GameEvent {
    readonly width: number;
    readonly height: number;
    readonly maxTick: number;
    readonly snakes: Snake[];
    readonly foods: Food[];
    constructor(tick: number, width: number, height: number, maxTick: number, snakes: Snake[], foods: Food[]) {
        super(tick);
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
        return state;
    }
}

export class TerminationEvent extends GameEvent {
    readonly snakeBasics: SnakeBasic[];
    constructor(tick: number, snakeBasics: SnakeBasic[]) {
        super(tick);
        this.snakeBasics = snakeBasics;
    }
    protected doApply(state: GameState | undefined) {
        if (!state) return undefined;
        state.tick = this.tick;
        for (let player_id = 0; player_id < this.snakeBasics.length; ++player_id) {
            state.snakes[player_id] = {...state.snakes[player_id], ...this.snakeBasics[player_id]};
        }
        return state;
    }
}
