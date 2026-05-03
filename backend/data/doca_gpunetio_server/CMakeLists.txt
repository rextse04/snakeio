find_package(PkgConfig REQUIRED)
pkg_check_modules(DOCA_GPUNETIO REQUIRED IMPORTED_TARGET doca-gpunetio)
pkg_check_modules(DOCA_COMMON REQUIRED IMPORTED_TARGET doca-common)
pkg_check_modules(DOCA_ETH REQUIRED IMPORTED_TARGET doca-eth)

add_library(doca_gpunetio_server_lib STATIC
    game.cpp
    transport.cpp)
target_include_directories(doca_gpunetio_server_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../gpu_server)
target_link_libraries(doca_gpunetio_server_lib PRIVATE
    gpu_server_logic_lib
    PkgConfig::DOCA_GPUNETIO
    PkgConfig::DOCA_COMMON
    PkgConfig::DOCA_ETH
    dl)

add_library(doca_gpunetio_server_packet_io_doca_gpunetio_test_lib INTERFACE)
target_link_libraries(doca_gpunetio_server_packet_io_doca_gpunetio_test_lib INTERFACE
    doca_gpunetio_server_lib
    crypt_packet_host_lib)
register_test(packet_io)