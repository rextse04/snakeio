import {FOOD_SIZE, SEGMENT_SIZE, SNAKE_BASIC_SIZE} from "./config.ts";
import {Packet} from "./packet.ts";
import GameState, {
    DeltaEvent, EventApplyResult, EventApplyResultType,
    Food,
    GameEvent,
    Point,
    SnakeBasic,
    SnapshotEvent,
    TerminationEvent,
    TickPolicyType
} from "./engine.ts";

export enum PacketType {
    DELTA = 0,
    SNAPSHOT = 1,
    TERMINATION = 3,
    UNKNOWN = -1,
    INVALID = -2
}
export class UnknownPacketEvent extends GameEvent {
    constructor(tick: number) {
        super(TickPolicyType.INCREMENTAL, tick);
    }
    protected doApply(_state: GameState | undefined): EventApplyResult {
        return {type: EventApplyResultType.NO_BASELINE};
    }
}
export class InvalidPacketEvent extends GameEvent {
    constructor(tick: number) {
        super(TickPolicyType.DEFINITION, tick);
    }
    protected doApply(_state: GameState | undefined): EventApplyResult {
        return {type: EventApplyResultType.INVALID};
    }
}
export type ParseResult =
    | {type: PacketType.DELTA, event: DeltaEvent}
    | {type: PacketType.SNAPSHOT, event: SnapshotEvent}
    | {type: PacketType.TERMINATION, event: TerminationEvent}
    | {type: PacketType.UNKNOWN, event: UnknownPacketEvent}
    | {type: PacketType.INVALID, event: InvalidPacketEvent};

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

export default function parsePacket({tick, data}: Packet, players?: number) : ParseResult {
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const type = view.getUint32(0, true);
    switch (type) {
        case PacketType.DELTA: {
            if (!players) return {
                type: PacketType.UNKNOWN,
                event: new UnknownPacketEvent(tick)
            };
            const snakeBasics: SnakeBasic[] = [];
            let offset = 4;
            for (let i = 0; i < players; i++) {
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

            return {
                type: PacketType.DELTA,
                event: new DeltaEvent(tick, snakeBasics, foodsAdded, foodsRemoved)
            };
        }
        case PacketType.SNAPSHOT: {
            const event = new SnapshotEvent(
                tick,
                view.getFloat32(4, true),
                view.getFloat32(8, true),
                view.getUint32(12, true),
                Array(view.getUint32(16, true)),
                []
            );

            let offset = 20;
            for (let i = 0; i < event.snakes.length; i++) {
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
                event.snakes[i] = {...basic, segments};
            }

            const foodsSize = view.getUint32(offset, true);
            offset += 4;
            for (let i = 0; i < foodsSize; i++) {
                const {food, nextOffset} = parseFood(view, offset);
                event.foods[i] = food;
                offset = nextOffset;
            }

            return {type: PacketType.SNAPSHOT, event};
            break;
        }
        case PacketType.TERMINATION: { // termination
            if (!players) return {
                type: PacketType.UNKNOWN,
                event: new UnknownPacketEvent(tick)
            };
            const maxTick = view.getUint32(4, true);

            const snakeBasics: SnakeBasic[] = [];
            let offset = 8;
            for (let i = 0; i < players; i++) {
                const {basic, nextOffset} = parseSnakeBasic(view, offset);
                snakeBasics[i] = basic;
                offset = nextOffset;
            }

            return {
                type: PacketType.TERMINATION,
                event: new TerminationEvent(tick, maxTick, snakeBasics)
            }
        }
        default: {
            console.error("Unknown packet type", type, data);
            return {
                type: PacketType.INVALID,
                event: new InvalidPacketEvent(tick)
            }
        }
    }
}