#define BOOST_TEST_MODULE doca_gpunetio_runtime_lifecycle
#include <boost/test/unit_test.hpp>

#include "doca_gpunetio_runtime.hpp"

#include <doca_error.h>

#include <cstdlib>

BOOST_AUTO_TEST_CASE(init_start_stop_shutdown_roundtrip) {
    BOOST_REQUIRE_EQUAL(snakeio::doca_gpunetio_runtime::init(), DOCA_SUCCESS);
    BOOST_REQUIRE_EQUAL(snakeio::doca_gpunetio_runtime::start_recv(nullptr), DOCA_SUCCESS);
    snakeio::doca_gpunetio_runtime::stop_recv(nullptr);
    snakeio::doca_gpunetio_runtime::shutdown();
    BOOST_CHECK(!snakeio::doca_gpunetio_runtime::started());
}
