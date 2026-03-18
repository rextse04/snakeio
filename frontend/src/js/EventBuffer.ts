import GameState, {EventApplyResultType, EventHandler, GameEvent} from "./engine.ts";
import React from "react";
import {TICK_RATE_MS} from "./config.ts";
import {OrderedMap} from "js-sdsl";

export default class EventBuffer implements EventHandler {
    #state: React.RefObject<GameState | undefined> | undefined;
    #buffer: OrderedMap<number, GameEvent>;
    tolerance: number;
    timeoutPerPacket: number;
    panic: () => void;
    resolve: () => void;
    onStateChange: (state: GameState | undefined) => void;
    constructor({
        tolerance = 5,
        timeoutPerPacket = TICK_RATE_MS * 2,
        panic = () => {},
        resolve = () => {},
        onStateChange = (_state: GameState | undefined) => {}
    }) {
        this.#buffer = new OrderedMap<number, GameEvent>();
        this.tolerance = tolerance;
        this.timeoutPerPacket = timeoutPerPacket;
        this.panic = panic;
        this.resolve = resolve;
        this.onStateChange = onStateChange;
    }
    bind(state: React.RefObject<GameState | undefined>) {
        this.#state = state;
    }
    handle(event: GameEvent) {
        let replay = false;
        while (true) {
            const event_tick = event.tick;
            const current_tick = this.#state!.current ? this.#state!.current.tick : -1;
            const result = event.apply(this.#state!.current);
            const type = result.type;
            switch (type) {
                case EventApplyResultType.SUCCESS: {
                    this.#state!.current = result.state;
                    this.onStateChange(this.#state!.current);
                    const current = this.#buffer.upperBound(event_tick);
                    let it = this.#buffer.begin();
                    while (!it.equals(current)) this.#buffer.eraseElementByIterator(it);
                    if (it.equals(this.#buffer.end())) {
                        this.resolve();
                        return type;
                    }
                    event = it.pointer[1];
                    replay = true;
                    continue;
                }
                case EventApplyResultType.NO_BASELINE:
                case EventApplyResultType.GAP: {
                    this.#buffer.setElement(event_tick, event!);
                    const gap = event_tick - current_tick;
                    if (gap > this.tolerance) {
                        this.panic();
                        return type;
                    }
                    if (!replay) setTimeout(() => {
                        if (!this.#buffer.find(event_tick).equals(this.#buffer.end())) this.panic();
                    }, this.timeoutPerPacket * gap);
                    return type;
                }
                case EventApplyResultType.STALE: {
                    return type;
                }
                case EventApplyResultType.INVALID: {
                    this.panic();
                    return type;
                }
            }
        }
    }
}