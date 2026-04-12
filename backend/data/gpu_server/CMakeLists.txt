add_library(gpu_server_lib STATIC game.cpp game.cu)
target_link_libraries(gpu_server_lib PRIVATE cpu_crypt_lib)

add_library(gpu_server_spatial_set_test_lib STATIC tests/spatial_set.cu)
register_test(spatial_set)