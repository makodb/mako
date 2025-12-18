// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * common/timestamp.h
 *   A transaction timestamp
 *
 **********************************************************************/

#ifndef _TIMESTAMP_H_
#define _TIMESTAMP_H_

#include "lib/assert.h"
#include "lib/message.h"

class Timestamp
{

public:
    // @safe - simple initialization
    Timestamp() : timestamp(0), id(0) { };
    // @safe - simple initialization
    Timestamp(uint64_t t) : timestamp(t), id(0) { };
    // @safe - simple initialization
    Timestamp(uint64_t t, uint64_t i) : timestamp(t), id(i) { };
    // @safe - default destructor
    ~Timestamp() { };
    // @safe - simple assignment
    void operator= (const Timestamp &t);
    // @safe - simple comparison
    bool operator== (const Timestamp &t) const;
    // @safe - simple comparison
    bool operator!= (const Timestamp &t) const;
    // @safe - simple comparison
    bool operator> (const Timestamp &t) const;
    // @safe - simple comparison
    bool operator< (const Timestamp &t) const;
    // @safe - simple comparison
    bool operator>= (const Timestamp &t) const;
    // @safe - simple comparison
    bool operator<= (const Timestamp &t) const;
    // @safe - simple increment
    Timestamp operator++ ();
    // @safe - simple check
    bool isValid() const;
    // @safe - simple getter
    uint64_t getID() const { return id; };
    // @safe - simple getter
    uint64_t getTimestamp() const { return timestamp; };
    // @safe - simple setter
    void setTimestamp(uint64_t t) { timestamp = t; };  

private:
	uint64_t timestamp;
	uint64_t id;
};

#endif  /* _TIMESTAMP_H_ */
