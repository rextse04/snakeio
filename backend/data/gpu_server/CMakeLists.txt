add_library(gpu_server_lib STATIC game.cpp game.cu)
target_link_libraries(gpu_server_lib PRIVATE crypt_gpu_lib)

add_library(gpu_server_spatial_set_test_lib STATIC tests/spatial_set.cu)
register_test(spatial_set)

add_library(gpu_server_game_test_lib STATIC tests/game.cpp)
target_link_libraries(gpu_server_game_test_lib PRIVATE gpu_server_lib)
register_test(game)

add_executable(gpu_server_packet_io_test tests/packet_io.cpp)
target_link_libraries(gpu_server_packet_io_test PRIVATE
    gpu_server_lib
    crypt_crypt_cpu_test_lib
    Boost::unit_test_framework)
