#pragma once

#include <memory>
#include <vector>

#include "lsm/iterator.h"

namespace lsm {

// Merges children that each iterate internal keys in order. Children must not
// contain equal keys (sequence numbers make every internal key unique). Stops
// at the first child error.
std::unique_ptr<Iterator> NewMergingIterator(
    std::vector<std::unique_ptr<Iterator>> children);

}  // namespace lsm
