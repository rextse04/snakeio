add_library(cpu_server_tick_core_lib STATIC tick_core.cpp session.cpp)
add_library(cpu_server_lib STATIC game.cpp session.cpp parse.cpp)
target_link_libraries(cpu_server_lib
        PRIVATE cpu_open_data_port_lib
        PRIVATE cpu_server_tick_core_lib
        PRIVATE crypt_lib)

add_library(cpu_server_spatial_set_independent_test_lib STATIC
        tests/spatial_set_independent.cpp
        tests/spatial_set_common.cpp)
register_test(spatial_set independent)
add_library(cpu_server_spatial_set_dependent_test_lib STATIC
        tests/spatial_set_dependent.cpp
        tests/spatial_set_common.cpp)
register_test(spatial_set dependent)

add_library(cpu_server_game_test_lib STATIC tests/game.cpp)
target_link_libraries(cpu_server_game_test_lib PUBLIC cpu_server_tick_core_lib)
register_test(game)

add_library(cpu_server_packet_io_cpu_test_lib INTERFACE)
target_link_libraries(cpu_server_packet_io_cpu_test_lib INTERFACE
        cpu_server_lib)
register_test(packet_io cpu)