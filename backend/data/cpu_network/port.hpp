#pragma once
#include <packet.hpp>
#include <stop_token>

namespace snakeio {
    void decode_port_stream(std::stop_token stop_token, int sock, void (*callback)(const data_packet&)) noexcept;
}
