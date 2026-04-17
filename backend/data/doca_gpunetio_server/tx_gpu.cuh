#pragma once
#include "doca_context.hpp"

namespace snakeio::doca_gpunetio_server {
    bool start_tx_worker(transport_context& ctx) noexcept;
    void stop_tx_worker(transport_context& ctx) noexcept;
}

