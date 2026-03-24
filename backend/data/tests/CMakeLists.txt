set(BOOST_USE_STATIC_LIBS OFF)

function (register_test)
    foreach (test_name IN LISTS ARGN)
        cmake_path(GET CMAKE_CURRENT_LIST_DIR FILENAME LIB_NAME)
        set(TEST_NAME ${LIB_NAME}_${test_name}_test)
        add_executable(${TEST_NAME}
                ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/${test_name}.cpp)
        target_link_libraries(${TEST_NAME} PRIVATE
                ${LIB_NAME}_test_lib
                Boost::unit_test_framework
        )
        target_compile_definitions(${TEST_NAME} PRIVATE
                SNAKEIO_TEST_CRYPT_PY=\"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/crypt.py\"
        )
    endforeach ()
endfunction()