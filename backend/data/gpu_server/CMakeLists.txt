#enable_language(CUDA)
set(CMAKE_CXX_STANDARD 20)

add_library(gpu_server_lib STATIC game.cpp)
target_link_libraries(gpu_server_lib PUBLIC cpu_crypt_lib)