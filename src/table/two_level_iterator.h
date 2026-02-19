#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include "lsm/iterator.h"

namespace lsm {

// Opens the second-level iterator an index entry's value refers to.
using BlockOpener =
    std::function<std::unique_ptr<Iterator>(std::string_view index_value)>;

// Iterates the concatenation of the blocks named by index_iter. Index keys
// must be >= every key in the block they point to. Stops at the first block
// whose iterator reports an error, and returns that error from status().
std::unique_ptr<Iterator> NewTwoLevelIterator(
    std::unique_ptr<Iterator> index_iter, BlockOpener open_block);

}  // namespace lsm
