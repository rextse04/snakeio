set(BOOST_USE_STATIC_LIBS OFF)

function (register_test test_name)
    cmake_path(GET CMAKE_CURRENT_LIST_DIR FILENAME LIB_NAME)
    set(TEST_PREFIX ${LIB_NAME}_${test_name})
    if (${ARGC} EQUAL 2)
        set(TEST_PREFIX ${TEST_PREFIX}_${ARGV1})
    endif()
    set(TEST_EXEC ${TEST_PREFIX}_test)
    add_executable(${TEST_EXEC}
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/${test_name}.cpp)
    target_link_libraries(${TEST_EXEC} PRIVATE
            ${TEST_EXEC}_lib
            Boost::unit_test_framework
    )
    target_compile_definitions(${TEST_EXEC} PRIVATE
            SNAKEIO_TEST_CRYPT_PY=\"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/crypt.py\"
    )
endfunction()