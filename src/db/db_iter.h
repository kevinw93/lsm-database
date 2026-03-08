#pragma once

#include <memory>

#include "db/dbformat.h"
#include "lsm/iterator.h"

namespace lsm {

// Turns an iterator over internal keys into the user-visible view at
// snapshot seq: one entry per live user key, newest version <= seq, with
// tombstoned keys hidden. pin keeps alive whatever the internal iterator
// reads from.
std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> internal,
                                        SequenceNumber seq,
                                        std::shared_ptr<const void> pin);

}  // namespace lsm
