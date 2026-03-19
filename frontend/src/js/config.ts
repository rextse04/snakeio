export const SNAKE_COLORS = [
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
    "#BA4A00",
    "#7D3C98",
    "#1F618D"
];
export const FOOD_COLORS = [
    "#ff0000",
    "#00ff00",
    "#0000ff",
    "#ffff00",
    "#ff00ff",
    "#00ffff",
    "#ffa500",
    "#800080",
    "#00a86b",
    "#ff69b4"
];
export function getSnakeColor(player_id: number) {
    return SNAKE_COLORS[player_id % SNAKE_COLORS.length];
}
export function getFoodColor(width: number) {
    return FOOD_COLORS[Math.floor(width) % FOOD_COLORS.length];
}

export const SNAKE_BASIC_SIZE = 24,
    SEGMENT_SIZE = 8,
    FOOD_SIZE = 12,
    PACKET_CHUNK_SIZE = 1024;

export default interface GameConfig {
    data_server_addr: string;
    tick_rate_ms: number;
    game_max_tick: number;
}