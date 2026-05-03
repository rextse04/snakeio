add_library(doca_tx_frame_lib STATIC doca_tx_frame.cpp)
target_include_directories(doca_tx_frame_lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_library(doca_gpunetio_server_lib STATIC game.cpp transport_doca.cpp doca_gpunetio_runtime.cpp doca_gpunetio_recv.cu doca_gpunetio_send.cu)
target_compile_definitions(doca_gpunetio_server_lib PRIVATE ALLOW_EXPERIMENTAL_API)
target_include_directories(doca_gpunetio_server_lib PRIVATE ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
target_link_libraries(doca_gpunetio_server_lib
        PRIVATE doca_tx_frame_lib
        PRIVATE CUDA::cudart
        PRIVATE gpu_server_logic_lib
        PRIVATE crypt_gpu_lib
        PRIVATE ${SNAKEIO_DOCA_GPUNETIO_LIB}
        PRIVATE ${SNAKEIO_DOCA_COMMON_LIB}
        PRIVATE ${SNAKEIO_DOCA_ETH_LIB}
        PRIVATE ${SNAKEIO_DOCA_VERBS_LIB}
        PRIVATE ${SNAKEIO_DOCA_FLOW_LIB}
        PRIVATE ${SNAKEIO_DOCA_FLOW_DEFINITIONS_LIB}
        PRIVATE ${SNAKEIO_DOCA_FLOW_PIPELINE_LOADER_LIB}
        PRIVATE ${SNAKEIO_DOCA_FLOW_INFO_COMP_LIB}
        PRIVATE ${SNAKEIO_DOCA_FLOW_TUNE_SERVER_LIB}
        PRIVATE ${SNAKEIO_LIBMLX5}
        PRIVATE ${SNAKEIO_LIBIBVERBS}
        PRIVATE ${SNAKEIO_LIBNVHWS}
        PRIVATE ZLIB::ZLIB)

set(DOCA_GPUNETIO_TEST_LINK_LIBS
        doca_gpunetio_server_lib
        CUDA::cudart
        gpu_server_logic_lib
        crypt_gpu_lib
        ${SNAKEIO_DOCA_GPUNETIO_LIB}
        ${SNAKEIO_DOCA_COMMON_LIB}
        ${SNAKEIO_DOCA_ETH_LIB}
        ${SNAKEIO_DOCA_VERBS_LIB}
        ${SNAKEIO_DOCA_FLOW_LIB}
        ${SNAKEIO_DOCA_FLOW_DEFINITIONS_LIB}
        ${SNAKEIO_DOCA_FLOW_PIPELINE_LOADER_LIB}
        ${SNAKEIO_DOCA_FLOW_INFO_COMP_LIB}
        ${SNAKEIO_DOCA_FLOW_TUNE_SERVER_LIB}
        ${SNAKEIO_LIBMLX5}
        ${SNAKEIO_LIBIBVERBS}
        ${SNAKEIO_LIBNVHWS}
        ZLIB::ZLIB
        Boost::unit_test_framework)

function (snakeio_add_doca_gpunetio_test exec_name source_file)
    add_executable(${exec_name} ${source_file})
    target_link_libraries(${exec_name} PRIVATE ${DOCA_GPUNETIO_TEST_LINK_LIBS})
    target_compile_definitions(${exec_name} PRIVATE BOOST_TEST_DYN_LINK)
    target_include_directories(${exec_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
endfunction()

snakeio_add_doca_gpunetio_test(doca_gpunetio_runtime_lifecycle_test tests/runtime_lifecycle.cpp)
snakeio_add_doca_gpunetio_test(doca_gpunetio_transport_smoke_test tests/transport_smoke.cpp)

add_executable(doca_gpunetio_tx_frame_test tests/tx_frame.cpp)
target_link_libraries(doca_gpunetio_tx_frame_test PRIVATE doca_tx_frame_lib Boost::unit_test_framework)
target_compile_definitions(doca_gpunetio_tx_frame_test PRIVATE BOOST_TEST_DYN_LINK)
target_include_directories(doca_gpunetio_tx_frame_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
