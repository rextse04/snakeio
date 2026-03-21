enable_language(CUDA)

add_library(gpu_server_lib STATIC game.cpp)
target_link_libraries(gpu_server_lib PUBLIC cpu_crypt_lib)

set_target_properties(gpu_server_lib PROPERTIES
        CUDA_STANDARD 20
        CUDA_STANDARD_REQUIRED ON
        CUDA_SEPARABLE_COMPILATION ON)