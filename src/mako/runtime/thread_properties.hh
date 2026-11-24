#pragma once
#include <map>
#include <string>

class HashWrapper {
    public:

    std::map<std::string, int> data;

    void set_tprops(std::string k, int v) {
        data[k] = v;
    }

    int get_tprops(std::string k) {
        if (data.find(k) != data.end()) {
            return data[k];
        } else {
            return -1;
        }
    }
} ;
