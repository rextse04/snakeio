add_library(doca_gpunetio_server_logic_lib STATIC
    rx_gpu.cu
    tx_gpu.cu
)

add_library(doca_gpunetio_server_lib STATIC
    game.cpp
    doca_context.cpp
    doca_gpunetio_server.cpp
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DOCA_GPUNETIO REQUIRED IMPORTED_TARGET doca-gpunetio)
pkg_check_modules(DOCA_ETH REQUIRED IMPORTED_TARGET doca-eth)
pkg_check_modules(DOCA_FLOW REQUIRED IMPORTED_TARGET doca-flow)
pkg_check_modules(DOCA_COMMON REQUIRED IMPORTED_TARGET doca-common)
pkg_check_modules(DPDK REQUIRED IMPORTED_TARGET doca-dpdk-bridge)

target_link_libraries(doca_gpunetio_server_logic_lib PRIVATE gpu_server_logic_lib)

target_link_libraries(doca_gpunetio_server_lib PRIVATE doca_gpunetio_server_logic_lib)
target_link_libraries(doca_gpunetio_server_lib PUBLIC
    PkgConfig::DOCA_GPUNETIO
    PkgConfig::DOCA_ETH
    PkgConfig::DOCA_FLOW
    PkgConfig::DOCA_COMMON
    PkgConfig::DPDK)

target_compile_options(doca_gpunetio_server_logic_lib PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-ccbin=g++-14>)
target_compile_options(doca_gpunetio_server_lib PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-DCUDA_HOST_COMPILER="g++-14" -Wno-deprecated-declarations>)

target_compile_definitions(doca_gpunetio_server_logic_lib PUBLIC SNAKEIO_DOCA_GPUNETIO_ENABLED=1)
target_compile_definitions(doca_gpunetio_server_lib PUBLIC SNAKEIO_DOCA_GPUNETIO_ENABLED=1)

add_library(doca_gpunetio_server_packet_io_doca_test_lib INTERFACE)
target_link_libraries(doca_gpunetio_server_packet_io_doca_test_lib INTERFACE
    doca_gpunetio_server_lib
    crypt_packet_host_lib)
register_test(packet_io doca)
