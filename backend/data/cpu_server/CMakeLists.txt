add_library(cpu_server_lib STATIC game.cpp packet.cpp)
add_library(cpu_server_test_lib STATIC packet.cpp tests/spatial_set.cpp)

register_test(
        crypt
        spatial_set
)