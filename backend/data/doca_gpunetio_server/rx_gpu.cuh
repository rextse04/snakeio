#pragma once
#include "doca_context.hpp"

namespace snakeio::doca_gpunetio_server {
    bool start_rx_worker(transport_context& ctx) noexcept;
    void stop_rx_worker(transport_context& ctx) noexcept;
}


