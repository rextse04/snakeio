add_library(gpu_server_logic_lib STATIC game_kernels.cu)
target_link_libraries(gpu_server_logic_lib PRIVATE crypt_gpu_lib)

add_library(gpu_server_lib STATIC game.cpp)
target_link_libraries(gpu_server_lib
        PRIVATE CUDA::cudart
        PRIVATE gpu_server_logic_lib)

add_library(gpu_server_spatial_set_test_lib STATIC tests/spatial_set.cu)
register_test(spatial_set)

add_library(gpu_server_game_test_lib STATIC tests/game.cpp)
target_link_libraries(gpu_server_game_test_lib PRIVATE gpu_server_lib)
register_test(game)

add_library(gpu_server_packet_io_gpu_test_lib INTERFACE)
target_link_libraries(gpu_server_packet_io_gpu_test_lib INTERFACE
    gpu_server_lib
    crypt_packet_host_lib)
register_test(packet_io gpu)
