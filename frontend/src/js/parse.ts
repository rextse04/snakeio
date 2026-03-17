import {FOOD_SIZE, SEGMENT_SIZE, SNAKE_BASIC_SIZE} from "./config.ts";
import {Packet} from "./packet.ts";
import GameState, {DeltaEvent, Food, GameEvent, Point, SnakeBasic, SnapshotEvent, TerminationEvent} from "./engine.ts";

export enum PacketType {
    DELTA = 0,
    SNAPSHOT = 1,
    TERMINATION = 3
}
export interface NetworkEvent extends GameEvent {
    type: PacketType;
}

function parseSnakeBasic(view: DataView, offset: number) {
    const speed = view.getFloat32(offset, true);
    const angle = view.getFloat32(offset + 4, true);
    const width = view.getFloat32(offset + 8, true);
    const length = view.getUint32(offset + 12, true);
    const score = view.getUint32(offset + 16, true);
    const alive = view.getUint8(offset + 20) !== 0;
    const human = view.getUint8(offset + 21) !== 0;
    return {
        basic: {speed, angle, width, length, score, alive, human} as SnakeBasic,
        nextOffset: offset + SNAKE_BASIC_SIZE
    };
}
function parseFood(view: DataView, offset: number) {
    const x = view.getFloat32(offset, true);
    const y = view.getFloat32(offset + 4, true);
    const radius = view.getFloat32(offset + 8, true);
    return {
        food: {pos: {x, y}, width: radius} as Food,
        nextOffset: offset + FOOD_SIZE
    };
}
export default function parsePacket(state: GameState | undefined, {tick, data}: Packet) : NetworkEvent | undefined {
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const type = view.getUint32(0, true);
    let event: GameEvent;
    switch (type) {
        case PacketType.DELTA: {
            if (!state) return undefined;
            const snakeBasics: SnakeBasic[] = [];
            let offset = 4;
            for (let i = 0; i < state.snakes.length; i++) {
                const {basic, nextOffset} = parseSnakeBasic(view, offset);
                snakeBasics[i] = basic;
                offset = nextOffset;
            }

            const foodsAdded: Food[] = [];
            const foodsAddedSize = view.getUint32(offset, true);
            offset += 4;
            for (let i = 0; i < foodsAddedSize; i++) {
                const {food, nextOffset} = parseFood(view, offset);
                foodsAdded[i] = food;
                offset = nextOffset;
            }

            const foodsRemoved: Point[] = [];
            const foodsRemovedSize = view.getUint32(offset, true);
            offset += 4;
            for (let i = 0; i < foodsRemovedSize; i++) {
                foodsRemoved[i] = {
                    x: view.getFloat32(offset, true),
                    y: view.getFloat32(offset + 4, true)
                };
                offset += 8;
            }

            event = new DeltaEvent(tick, snakeBasics, foodsAdded, foodsRemoved) as unknown as NetworkEvent;
            break;
        }
        case PacketType.SNAPSHOT: {
            const snapshot = new SnapshotEvent(
                tick,
                view.getFloat32(4, true),
                view.getFloat32(8, true),
                view.getUint32(12, true),
                Array(view.getUint32(16, true)),
                []
            );

            let offset = 20;
            for (let i = 0; i < snapshot.snakes.length; i++) {
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
                snapshot.snakes[i] = {...basic, segments};
            }

            const foodsSize = view.getUint32(offset, true);
            offset += 4;
            for (let i = 0; i < foodsSize; i++) {
                const {food, nextOffset} = parseFood(view, offset);
                snapshot.foods[i] = food;
                offset = nextOffset;
            }

            event = snapshot;
            break;
        }
        case PacketType.TERMINATION: { // termination
            if (!state) return undefined;
            const snakeBasics: SnakeBasic[] = [];
            let offset = 4;
            for (let i = 0; i < state.snakes.length; i++) {
                const {basic, nextOffset} = parseSnakeBasic(view, offset);
                snakeBasics[i] = {...state.snakes[i], ...basic};
                offset = nextOffset;
            }

            event = new TerminationEvent(tick, snakeBasics);
            break;
        }
        default: {
            console.error("Unknown packet type", type, data);
            return undefined;
        }
    }
    const out = event as unknown as NetworkEvent;
    out.type = type;
    return out;
}