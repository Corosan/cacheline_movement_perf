# cacheline_movement_perf

How long is to transfer one cache line from one CPU to another? The program has a number of tests
for measuring this latency.

It should be possible to compile it on any posix-like system having cmake ver>=3.8 and any compiler
supporting c++17 like:

    mkdir build && cd build
    cmake -DCMAKE_BUILD_TYPE=Release ..
