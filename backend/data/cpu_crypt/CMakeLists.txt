add_library(cpu_crypt_lib STATIC packet.cpp)

add_library(cpu_crypt_crypt_test_lib ALIAS cpu_crypt_lib)
register_test(crypt)