find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

find_path(SNAKEIO_DOCA_INCLUDE_DIR doca_gpunetio.h
    PATHS
    /opt/mellanox/doca/include
)

find_library(SNAKEIO_DOCA_GPUNETIO_LIB doca_gpunetio PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
find_library(SNAKEIO_DOCA_COMMON_LIB doca_common PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
find_library(SNAKEIO_DOCA_ETH_LIB doca_eth PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
find_library(SNAKEIO_DOCA_VERBS_LIB doca_verbs PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
find_library(SNAKEIO_DOCA_FLOW_LIB doca_flow PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
find_library(SNAKEIO_DOCA_FLOW_DEF_LIB doca_flow_definitions PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
find_library(SNAKEIO_DOCA_FLOW_PL_LIB doca_flow_pipeline_loader PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
find_library(SNAKEIO_DOCA_FLOW_INF_LIB doca_flow_info_comp PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
find_library(SNAKEIO_DOCA_FLOW_TUNE_LIB doca_flow_tune_server PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)

find_library(SNAKEIO_LIB_MLX5 mlx5 PATHS /usr/lib/x86_64-linux-gnu /opt/mellanox/doca/lib/x86_64-linux-gnu)
find_library(SNAKEIO_LIB_IBVERBS ibverbs PATHS /usr/lib/x86_64-linux-gnu /opt/mellanox/doca/lib/x86_64-linux-gnu)
find_library(SNAKEIO_LIB_NVHWS nvhws PATHS /usr/lib/x86_64-linux-gnu)

add_library(doca_gpunetio_server_lib STATIC
    game.cpp
    doca_gpunetio_runtime.cpp
    doca_gpunetio_net.cpp
    doca_gpunetio_rx.cu
    doca_gpunetio_tx.cu
    doca_gpunetio_recv_sample.c
)
set_source_files_properties(doca_gpunetio_recv_sample.c PROPERTIES LANGUAGE C)

target_link_libraries(doca_gpunetio_server_lib
    PRIVATE CUDA::cudart
    PRIVATE gpu_server_logic_lib
    PRIVATE ZLIB::ZLIB
)

if (SNAKEIO_DOCA_INCLUDE_DIR)
    target_include_directories(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_DOCA_INCLUDE_DIR})
endif()
target_include_directories(doca_gpunetio_server_lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# nvcc can fail to resolve <math.h> when /usr/include is passed as -isystem; prefer plain -I.
if (CMAKE_CUDA_COMPILER)
    target_compile_options(doca_gpunetio_server_lib BEFORE PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-I/usr/include>)
endif()

if (SNAKEIO_DOCA_GPUNETIO_LIB)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_DOCA_GPUNETIO_LIB})
endif()
if (SNAKEIO_DOCA_COMMON_LIB)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_DOCA_COMMON_LIB})
endif()
if (SNAKEIO_DOCA_ETH_LIB)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_DOCA_ETH_LIB})
endif()
if (SNAKEIO_DOCA_VERBS_LIB)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_DOCA_VERBS_LIB})
endif()
if (SNAKEIO_DOCA_FLOW_LIB)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_DOCA_FLOW_LIB})
endif()
if (SNAKEIO_DOCA_FLOW_DEF_LIB)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_DOCA_FLOW_DEF_LIB})
endif()
if (SNAKEIO_DOCA_FLOW_PL_LIB)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_DOCA_FLOW_PL_LIB})
endif()
if (SNAKEIO_DOCA_FLOW_INF_LIB)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_DOCA_FLOW_INF_LIB})
endif()
if (SNAKEIO_DOCA_FLOW_TUNE_LIB)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_DOCA_FLOW_TUNE_LIB})
endif()
if (SNAKEIO_LIB_MLX5)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_LIB_MLX5})
endif()
if (SNAKEIO_LIB_IBVERBS)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_LIB_IBVERBS})
endif()
if (SNAKEIO_LIB_NVHWS)
    target_link_libraries(doca_gpunetio_server_lib PRIVATE ${SNAKEIO_LIB_NVHWS})
endif()

target_link_libraries(doca_gpunetio_server_lib PRIVATE Threads::Threads)

if (CMAKE_CUDA_COMPILER AND SNAKEIO_DOCA_GPUNETIO_LIB)
    add_executable(doca_gpunetio_packet_io_remote_ipv4_test
        tests/packet_io_doca_remote_ipv4.cpp
        tests/packet_io_doca_remote_stub.cu)
    set_property(TARGET doca_gpunetio_packet_io_remote_ipv4_test PROPERTY CUDA_SEPARABLE_COMPILATION ON)
    target_compile_definitions(doca_gpunetio_packet_io_remote_ipv4_test PRIVATE
        "SNAKEIO_DOCA_REMOTE_AGENT=\"${CMAKE_CURRENT_SOURCE_DIR}/scripts/doc_packet_io_remote_agent.py\"")
    target_link_libraries(doca_gpunetio_packet_io_remote_ipv4_test PRIVATE
        doca_gpunetio_server_lib
        crypt_packet_host_lib
        Boost::unit_test_framework
        CUDA::cudart)

    add_executable(doca_gpunetio_toy_gpu_udp_reach toys/gpu_udp_reach.cpp)
    target_include_directories(doca_gpunetio_toy_gpu_udp_reach PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if (SNAKEIO_DOCA_INCLUDE_DIR)
        target_include_directories(doca_gpunetio_toy_gpu_udp_reach PRIVATE ${SNAKEIO_DOCA_INCLUDE_DIR})
    endif()
    target_link_libraries(doca_gpunetio_toy_gpu_udp_reach PRIVATE doca_gpunetio_server_lib CUDA::cudart)

    add_executable(doca_gpunetio_toy_gpu_udp_egress_reach toys/gpu_udp_egress_reach.cpp)
    target_include_directories(doca_gpunetio_toy_gpu_udp_egress_reach PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if (SNAKEIO_DOCA_INCLUDE_DIR)
        target_include_directories(doca_gpunetio_toy_gpu_udp_egress_reach PRIVATE ${SNAKEIO_DOCA_INCLUDE_DIR})
    endif()
    target_compile_definitions(doca_gpunetio_toy_gpu_udp_egress_reach PRIVATE
        "SNAKEIO_DOCA_EGRESS_LISTEN_SCRIPT=\"${CMAKE_CURRENT_SOURCE_DIR}/scripts/doc_udp_egress_listen_remote.py\"")
    target_include_directories(doca_gpunetio_toy_gpu_udp_egress_reach PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(doca_gpunetio_toy_gpu_udp_egress_reach PRIVATE doca_gpunetio_server_lib CUDA::cudart)
endif()
