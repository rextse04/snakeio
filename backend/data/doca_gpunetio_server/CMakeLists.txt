add_library(doca_gpunetio_server_lib STATIC game.cpp transport_doca.cpp)
target_link_libraries(doca_gpunetio_server_lib
        PRIVATE CUDA::cudart
        PRIVATE gpu_server_logic_lib
        PRIVATE crypt_gpu_lib
        PRIVATE ${SNAKEIO_DOCA_GPUNETIO_LIB}
        PRIVATE ${SNAKEIO_DOCA_COMMON_LIB}
        PRIVATE ${SNAKEIO_DOCA_ETH_LIB})

add_library(doca_gpunetio_server_packet_io_test_lib INTERFACE)
target_link_libraries(doca_gpunetio_server_packet_io_test_lib INTERFACE
        crypt_packet_host_lib)
register_test(packet_io)
