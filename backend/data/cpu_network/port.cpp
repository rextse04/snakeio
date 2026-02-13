#include "port.hpp"
#include <config.hpp>
#include <arpa/inet.h>
#include <cstring>

void snakeio::decode_port_stream(std::stop_token stop_token, int sock, void(*callback)(const data_packet &)) noexcept {
    std::byte buffer[packet_max_size];
    sockaddr_in client_addr;
    while (!stop_token.stop_requested()) {
        socklen_t client_addr_len = sizeof(client_addr);
        const ssize_t recv_len = recvfrom(sock, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
        if (recv_len < data_packet::header_size) continue;
        data_packet packet;
        std::memcpy(&packet.session_id, buffer, sizeof(id_t));
        packet.session_id = ntohl(packet.session_id);
        std::memcpy(&packet.player_id, buffer + sizeof(id_t), sizeof(id_t));
        packet.player_id = ntohl(packet.player_id);
        std::memcpy(packet.tag.data(), buffer + sizeof(id_t) * 2, sizeof(packet.tag));
        packet.ciphertext_size = recv_len - data_packet::header_size;
        std::memcpy(packet.ciphertext_buffer.data(), buffer + data_packet::header_size, packet.ciphertext_size);
        callback(packet);
    }
}
