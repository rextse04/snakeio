import {useRef, useState} from "react";

export function useStateRef<T>(initialValue: T) {
    const [state, setState] = useState(initialValue);
    const stateRef = useRef(state);
    stateRef.current = state;
    return [state, stateRef, setState] as const;
}

export function angleDelta(b: number, a: number) {
    let d = b - a;
    while (d < -Math.PI) d += 2 * Math.PI;
    while (d > Math.PI) d -= 2 * Math.PI;
    return d;
}
export function clamp(value: number, min: number, max: number) {
    return Math.min(Math.max(value, min), max);
}