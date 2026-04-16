#pragma once

#include "impl.cuh"

namespace snakeio::gpu {
    void launch_add_session(game::impl& impl_, const add_session_req& req) noexcept;

    void launch_tick(game::impl& impl_, id_t session_id) noexcept;
}

