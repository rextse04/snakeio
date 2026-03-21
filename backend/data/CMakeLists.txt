cmake_minimum_required(VERSION 4.2)
project(snakeio)

set(CMAKE_CXX_STANDARD 26)

include_directories(.)
find_package(Boost REQUIRED COMPONENTS unit_test_framework)
add_subdirectory(tests)
add_subdirectory(cpu_crypt)

function (add_server server_name)
    add_executable(${server_name}_o main.cpp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${server_name})
    target_link_libraries(${server_name}_o PRIVATE ${server_name}_lib)
    target_link_libraries(${server_name}_o PRIVATE atomic)
endfunction()
add_server(cpu_server)