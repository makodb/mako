#include "deptran/s_main.h"
#include <iostream>
#include <vector>
#include <sys/time.h>
#include <thread>
#include <string>
#include <cstring>
#include <unistd.h>

int main(int argc, char* argv[]){
    if (argc < 2) return -1;

    unsigned int is_server = atoi(argv[1]) ;
    
    if (is_server) {  // this thread should attach to Rolis
        std::cout << "start a server on 10010\n";
        nc_setup_server();
        sleep(1); // wait for server starts
        nc_mimic_obtain_requests();
        sleep(30); 
    } else {
        sleep(1) ; // wait for server start
        std::cout << "start the benchmark\n";
        nc_setup_bench(100*10000);
    }
    return 0;
}