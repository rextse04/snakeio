export const CONTROL_PLANE_EXT_PORT = 50000,
    CONTROL_PLANE_INT_PORT = 50001,
    DATA_PLANE_INT_PORT = 50002,
    DATA_PLANE_EXT_PORT = 50003,
    SESSION_TOKEN_LEN = 5,
    KEY_LEN = 32,
    GAME_MAX_PLAYERS = 16,
    GAME_MAX_TICK = 15000;
export const game_config = {
    data_server_addr: "localhost:50003",
    tick_rate_ms: 20,
    game_max_tick: GAME_MAX_TICK
};