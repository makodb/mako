// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * common/timestamp.cc:
 *   A transaction timestamp implementation
 *
 **********************************************************************/

#include "lib/timestamp.h"

void
Timestamp::operator=(const Timestamp &t)
{
    timestamp = t.timestamp;
    id = t.id;
}

Timestamp
Timestamp::operator++()
{
    timestamp++;
    return *this;
}

bool
Timestamp::operator==(const Timestamp &t) const
{
    return timestamp == t.timestamp && id == t.id;
}

bool
Timestamp::operator!=(const Timestamp &t) const
{
    return timestamp != t.timestamp || id != t.id;
}

bool
Timestamp::operator>(const Timestamp &t) const
{
    return (timestamp == t.timestamp) ? id > t.id : timestamp > t.timestamp; 
}

bool
Timestamp::operator<(const Timestamp &t) const
{
    return (timestamp == t.timestamp) ? id < t.id : timestamp < t.timestamp; 
}

bool
Timestamp::operator>=(const Timestamp &t) const
{
    return (timestamp == t.timestamp) ? id >= t.id : timestamp >= t.timestamp; 
}

bool
Timestamp::operator<=(const Timestamp &t) const
{
    return (timestamp == t.timestamp) ? id <= t.id : timestamp <= t.timestamp; 
}

bool
Timestamp::isValid() const
{
    return timestamp > 0 && id > 0;
}
