add_library(cpu_server_lib STATIC game.cpp session.cpp parse.cpp)
target_link_libraries(cpu_server_lib PRIVATE cpu_crypt_lib)

add_library(cpu_server_spatial_set_independent_test_lib STATIC
        tests/spatial_set_independent.cpp
        tests/spatial_set_common.cpp)
register_test(spatial_set independent)
add_library(cpu_server_spatial_set_dependent_test_lib STATIC
        tests/spatial_set_dependent.cpp
        tests/spatial_set_common.cpp)
register_test(spatial_set dependent)