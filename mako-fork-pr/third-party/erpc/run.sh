rm -rf CMakeFiles cmake_install.cmake CMakeCache.txt
#cmake . -DTRANSPORT=dpdk -DAZURE=off -DPERF=ON -DLOG_LEVEL=none
#cmake . -DTRANSPORT=infiniband -DROCE=on -DPERF=ON
cmake . -DTRANSPORT=fake -DROCE=off -DPERF=off
make -j10
