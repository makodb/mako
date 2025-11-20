/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2013 President and Fellows of Harvard College
 * Copyright (c) 2012-2013 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */

// RustyCpp Safety Status: str.cc
// This file defines static constants for Str
// @safe - Static constant initialization with compile-time known values

#include "str.hh"
namespace lcdf {

// @safe - Static constant for maximum key value (257 bytes of 0xFF)
// SAFETY: Compile-time initialized constant string, immutable after initialization
const Str Str::maxkey("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
                      "\xFF", 257);

} // namespace lcdf
