#define BOOST_TEST_MODULE doca_gpunetio_transport_smoke
#include <boost/test/unit_test.hpp>

#include "transport_doca.hpp"

#include <cstdlib>

BOOST_AUTO_TEST_CASE(ensure_ingress_then_shutdown) {
    BOOST_REQUIRE(snakeio::doca_transport::ensure_ingress_path_started());
    snakeio::doca_transport::shutdown_ingress_path();
    BOOST_CHECK(true);
}
