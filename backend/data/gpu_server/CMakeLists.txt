add_library(gpu_server_lib STATIC impl.cu parse.cu port.cu game.cu tick_core.cu)
target_link_libraries(gpu_server_lib PRIVATE cpu_crypt_lib)

add_library(gpu_server_spatial_set_test_lib STATIC tests/spatial_set.cu)
register_test(spatial_set)