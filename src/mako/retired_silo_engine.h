#pragma once

// The original Silo transaction engine is retained as source-only history.
// Production Mako uses storage/mbta_wrapper.hh with STO Transaction and
// MassTrans. This fail-closed guard is intentional: do not add an escape
// macro or compile the archived engine into a Mako target.
#error "The original Silo transaction engine is retired; use STO/MassTrans through storage/mbta_wrapper.hh"
