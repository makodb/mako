// wrapper.h
#ifndef WRAPPER_H
#define WRAPPER_H

#include <iostream>
#include <string>

using namespace std;

class Wrapper {
public:
    Wrapper() = default;
    ~Wrapper() = default;

    bool init();
    int sendtoqueue(string request);
    string recvfromqueue(int reqId);
};

#endif // WRAPPER_H
