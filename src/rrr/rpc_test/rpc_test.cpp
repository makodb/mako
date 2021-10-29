//
// Created by weihshen on 10/26/21.
//
#include "iostream"
using namespace std;

// a server and client
int main(int argc, char **argv) {
    std::cout << "rpc_test start...\n" ;

    // FLAG, send a (char*)logs
    rrr::Client *client = new rrr::Client(new rrr::PollMgr());
    client->connect((char*)"127.0.0.1");
    return 0;
}