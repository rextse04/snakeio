# DOCA GPUNetIO build reminder
Use g++-14 or above.

Use the following CMake options for CUDA:

-DCMAKE_CUDA_COMPILER="/usr/local/cuda-12.8/bin/nvcc"
-DCMAKE_CUDA_FLAGS="-arch=sm_86"