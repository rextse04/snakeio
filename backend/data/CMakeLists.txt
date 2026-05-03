cmake_minimum_required(VERSION 4.2)
project(snakeio)

include(CheckLanguage)

set(CMAKE_CXX_STANDARD 26)
check_language(CUDA)
if (CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    find_package(CUDAToolkit)
    set(CMAKE_CUDA_STANDARD 20)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    set(CMAKE_CUDA_SEPARABLE_COMPILATION ON)
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --extended-lambda --expt-relaxed-constexpr")

    include_directories(cccl/thrust)
    include_directories(cccl/libcudacxx/include)
    include_directories(cccl/cub)

    option(SNAKEIO_ENABLE_DOCA_GPUNETIO_SERVER "Build the DOCA GPUNetIO backend" ON)
    if (SNAKEIO_ENABLE_DOCA_GPUNETIO_SERVER)
        find_path(SNAKEIO_DOCA_INCLUDE_DIR
                NAMES doca_gpunetio.h
                PATHS /opt/mellanox/doca/include)
        find_library(SNAKEIO_DOCA_GPUNETIO_LIB
                NAMES doca_gpunetio
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_DOCA_COMMON_LIB
                NAMES doca_common
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_DOCA_ETH_LIB
                NAMES doca_eth
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_DOCA_VERBS_LIB
                NAMES doca_verbs
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_DOCA_FLOW_LIB
                NAMES doca_flow
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_DOCA_FLOW_DEFINITIONS_LIB
                NAMES doca_flow_definitions
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_DOCA_FLOW_PIPELINE_LOADER_LIB
                NAMES doca_flow_pipeline_loader
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_DOCA_FLOW_INFO_COMP_LIB
                NAMES doca_flow_info_comp
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_DOCA_FLOW_TUNE_SERVER_LIB
                NAMES doca_flow_tune_server
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_LIBMLX5
                NAMES mlx5
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_LIBIBVERBS
                NAMES ibverbs
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)
        find_library(SNAKEIO_LIBNVHWS
                NAMES nvhws
                PATHS /opt/mellanox/doca/lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)
        if (NOT SNAKEIO_DOCA_INCLUDE_DIR OR
            NOT SNAKEIO_DOCA_GPUNETIO_LIB OR
            NOT SNAKEIO_DOCA_COMMON_LIB OR
            NOT SNAKEIO_DOCA_ETH_LIB OR
            NOT SNAKEIO_DOCA_VERBS_LIB OR
            NOT SNAKEIO_DOCA_FLOW_LIB OR
            NOT SNAKEIO_DOCA_FLOW_DEFINITIONS_LIB OR
            NOT SNAKEIO_DOCA_FLOW_PIPELINE_LOADER_LIB OR
            NOT SNAKEIO_DOCA_FLOW_INFO_COMP_LIB OR
            NOT SNAKEIO_DOCA_FLOW_TUNE_SERVER_LIB OR
            NOT SNAKEIO_LIBMLX5 OR
            NOT SNAKEIO_LIBIBVERBS OR
            NOT SNAKEIO_LIBNVHWS)
            message(FATAL_ERROR
                    "SNAKEIO_ENABLE_DOCA_GPUNETIO_SERVER=ON, but required DOCA headers/libraries were not found."
                    " Expected doca_gpunetio.h and libs: doca_gpunetio, doca_common, doca_eth, doca_verbs,"
                    " doca_flow (+ doca_flow_* helper libs), mlx5, ibverbs, nvhws.")
        endif()
        include_directories(${SNAKEIO_DOCA_INCLUDE_DIR})
    endif()
endif()

include_directories(.)
find_package(Boost REQUIRED COMPONENTS unit_test_framework)
find_package(ZLIB REQUIRED)
add_subdirectory(tests)
add_subdirectory(crypt)

function (add_server server_name)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${server_name})
    add_executable(${server_name}_o main.cpp)
    target_link_libraries(${server_name}_o PRIVATE ${server_name}_lib)
endfunction()
add_server(cpu_server)
if (CMAKE_CUDA_COMPILER)
    add_server(gpu_server)
    if (SNAKEIO_ENABLE_DOCA_GPUNETIO_SERVER)
        add_server(doca_gpunetio_server)
    endif()
endif()