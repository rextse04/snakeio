add_library(cpu_server_lib STATIC game.cpp session.cpp parse.cpp)
target_link_libraries(cpu_server_lib PRIVATE cpu_crypt_lib cpu_spatial_set_lib)

add_library(cpu_server_test_lib STATIC tests/spatial_set.cpp)
register_test(spatial_set)