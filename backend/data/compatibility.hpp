/* Include this file after all includes involving std or cuda::std. */
#pragma once

namespace snakeio {
#ifdef __CUDACC__
    namespace stdc = cuda::std;
#else
#define __host__
#define __device__
    namespace stdc = std;
#endif
}