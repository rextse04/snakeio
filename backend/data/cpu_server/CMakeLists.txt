add_library(cpu_server_lib STATIC game.cpp session.cpp parse.cpp)
target_link_libraries(cpu_server_lib PUBLIC cpu_crypt_lib)
add_library(cpu_server_test_lib STATIC tests/spatial_set.cpp)

register_test(spatial_set)