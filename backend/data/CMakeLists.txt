cmake_minimum_required(VERSION 4.2)
project(snakeio)

set(CMAKE_CXX_STANDARD 26)

include_directories(.)

file(GLOB SERVER_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/*_server")
foreach(dir ${SERVER_DIRS})
    if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${dir}")
        add_executable(${dir} main.cpp game.cpp)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/${dir})
        target_link_libraries(${dir} PRIVATE ${dir}_lib)
    endif()
endforeach()