// C++
#include <algorithm>

// Boost
#include <boost/scope_exit.hpp>

// App
#include "nbtree.h"
#include "akumuli_version.h"
#include "status_util.h"

/** NOTE:
  * BlockStore should have cache. This cache should be implemented on
  * _this_ level because importance of each block should be taken into
  * account. Eviction algorithm should check whether or not block was
  * actually deleted. Alg. for this:
  * 1. Get weak ptr for the block.
  * 2. Remove block from cache.
  * 3. Try to lock weak ptr. On success try to remove another block.
  * 4. Otherwise we done.
  */


namespace Akumuli {
namespace StorageEngine {

//! This value represents empty addr. It's too large to be used as a real block addr.
static const LogicAddr EMPTY = std::numeric_limits<LogicAddr>::max();

static SubtreeRef* subtree_cast(u8* p) {
    return reinterpret_cast<SubtreeRef*>(p);
}

static SubtreeRef const* subtree_cast(u8 const* p) {
    return reinterpret_cast<SubtreeRef const*>(p);
}

//! Initialize object from leaf node
static aku_Status init_subtree_from_leaf(const NBTreeLeaf& leaf, SubtreeRefPayload& out) {
    std::vector<aku_Timestamp> ts;
    std::vector<double> xs;
    aku_Status status = leaf.read_all(&ts, &xs);
    if (status != AKU_SUCCESS) {
        return status;
    }
    if (xs.empty()) {
        // Can't add empty leaf node to the node!
        return AKU_ENO_DATA;
    }
    double min = std::numeric_limits<double>::max();
    double max = std::numeric_limits<double>::min();
    double sum = 0;
    for (auto x: xs) {
        min = std::min(min, x);
        max = std::max(max, x);
        sum = sum + x;
    }
    out.max = max;
    out.min = min;
    out.sum = sum;
    out.begin = ts.front();
    out.end = ts.back();
    out.count = xs.size();
    return AKU_SUCCESS;
}

static aku_Status init_subtree_from_subtree(const NBTreeSuperblock& node, SubtreeRefPayload& backref) {
    std::vector<SubtreeRef> refs;
    aku_Status status = node.read_all(&refs);
    if (status != AKU_SUCCESS) {
        return status;
    }
    backref.begin = refs.front().begin;
    backref.end = refs.back().end;
    backref.count = 0;
    backref.sum = 0;
    double min = std::numeric_limits<double>::max();
    double max = std::numeric_limits<double>::min();
    for (const SubtreeRef& sref: refs) {
        backref.count += sref.count;
        backref.sum   += sref.sum;
        min = std::min(min, sref.min);
        max = std::max(max, sref.max);
    }
    backref.min = min;
    backref.max = max;
    return AKU_SUCCESS;
}

NBTreeLeaf::NBTreeLeaf(aku_ParamId id, LogicAddr prev, u16 fanout_index)
    : prev_(prev)
    , buffer_(AKU_BLOCK_SIZE, 0)
    , writer_(id, buffer_.data() + sizeof(SubtreeRef), AKU_BLOCK_SIZE - sizeof(SubtreeRef))
    , fanout_index_(fanout_index)
{
    SubtreeRef* subtree = subtree_cast(buffer_.data());
    subtree->addr = prev;
    subtree->level = 0;  // Leaf node
    subtree->id = id;
    subtree->version = AKUMULI_VERSION;
    subtree->payload_size = 0;
    subtree->fanout_index = fanout_index;
    // values that should be updated by insert
    subtree->begin = std::numeric_limits<aku_Timestamp>::max();
    subtree->end = 0;
    subtree->count = 0;
    subtree->min = std::numeric_limits<double>::max();
    subtree->max = std::numeric_limits<double>::min();
    subtree->sum = 0;
}

NBTreeLeaf::NBTreeLeaf(std::shared_ptr<BlockStore> bstore, LogicAddr curr, LeafLoadMethod load)
    : prev_(EMPTY)
{
    aku_Status status;
    std::shared_ptr<Block> block;
    std::tie(status, block) = bstore->read_block(curr);
    if (status != AKU_SUCCESS) {
        AKU_PANIC("Can't read block - " + StatusUtil::str(status));
    }
    if (load == LeafLoadMethod::FULL_PAGE_LOAD) {
        buffer_.reserve(block->get_size());
        std::copy(block->get_data(), block->get_data() + block->get_size(),
                  std::back_inserter(buffer_));
    } else {
        buffer_.reserve(sizeof(SubtreeRef));
        std::copy(block->get_data(), block->get_data() + sizeof(SubtreeRef),
                  std::back_inserter(buffer_));
    }
    SubtreeRef* subtree = subtree_cast(buffer_.data());
    prev_ = subtree->addr;
    fanout_index_ = subtree->fanout_index;
}

size_t NBTreeLeaf::nelements() {
    SubtreeRef* subtree = subtree_cast(buffer_.data());
    return subtree->count;
}


std::tuple<aku_Timestamp, aku_Timestamp> NBTreeLeaf::get_timestamps() const {
    SubtreeRef const* subtree = subtree_cast(buffer_.data());
    return std::make_tuple(subtree->begin, subtree->end);
}

LogicAddr NBTreeLeaf::get_prev_addr() const {
    // Should be set correctly no metter how NBTreeLeaf was created.
    return prev_;
}


aku_Status NBTreeLeaf::read_all(std::vector<aku_Timestamp>* timestamps,
                                std::vector<double>* values) const
{
    if (buffer_.size() == sizeof(SubtreeRef)) {
        // Error. Page is not fully loaded
        return AKU_ENO_DATA;
    }
    int windex = writer_.get_write_index();
    DataBlockReader reader(buffer_.data() + sizeof(SubtreeRef), buffer_.size());
    size_t sz = reader.nelements();
    timestamps->reserve(sz);
    values->reserve(sz);
    for (size_t ix = 0; ix < sz; ix++) {
        aku_Status status;
        aku_Timestamp ts;
        double value;
        std::tie(status, ts, value) = reader.next();
        if (status != AKU_SUCCESS) {
            return status;
        }
        timestamps->push_back(ts);
        values->push_back(value);
    }
    // Read tail elements from `writer_`
    if (windex != 0) {
        writer_.read_tail_elements(timestamps, values);
    }
    return AKU_SUCCESS;
}

aku_Status NBTreeLeaf::append(aku_Timestamp ts, double value) {
    aku_Status status = writer_.put(ts, value);
    if (status == AKU_SUCCESS) {
        SubtreeRef* subtree = subtree_cast(buffer_.data());
        subtree->end = ts;
        if (subtree->count == 0) {
            subtree->begin = ts;
        }
        subtree->count++;
        subtree->sum += value;
        subtree->max = std::max(subtree->max, value);
        subtree->min = std::min(subtree->min, value);
    }
    return status;
}

std::tuple<aku_Status, LogicAddr> NBTreeLeaf::commit(std::shared_ptr<BlockStore> bstore) {
    size_t size = writer_.commit();
    SubtreeRef* subtree = subtree_cast(buffer_.data());
    subtree->payload_size = size;
    if (prev_ != EMPTY) {
        NBTreeLeaf prev(bstore, prev_, LeafLoadMethod::FULL_PAGE_LOAD);
        // acquire info
        aku_Status status = init_subtree_from_leaf(prev, *subtree);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, EMPTY);
        }
    } else {
        // count = 0 and addr = EMPTY indicates that there is
        // no link to previous node.
        subtree->count = 0;
        subtree->addr  = EMPTY;
        // Invariant: fanout index should be 0 in this case.
        assert(fanout_index_ == 0);
    }
    subtree->version = AKUMULI_VERSION;
    subtree->level = 0;
    subtree->fanout_index = fanout_index_;
    return bstore->append_block(buffer_.data());
}


// //////////////////////// //
//     NBTreeSuperblock     //
// //////////////////////// //

NBTreeSuperblock::NBTreeSuperblock(aku_ParamId id, LogicAddr prev, u16 fanout, u16 lvl)
    : buffer_(AKU_BLOCK_SIZE, 0)
    , id_(id)
    , write_pos_(0)
    , fanout_index_(fanout)
    , level_(lvl)
    , prev_(prev)
    , immutable_(false)
{
}

NBTreeSuperblock::NBTreeSuperblock(LogicAddr addr, std::shared_ptr<BlockStore> bstore)
    : buffer_(AKU_BLOCK_SIZE, 0)
    , immutable_(true)
{
    // Read content from bstore
    aku_Status status;
    std::shared_ptr<Block> block;
    std::tie(status, block) = bstore->read_block(addr);
    if (status != AKU_SUCCESS) {
        AKU_PANIC("Bad arg, can't read data from block store, " + StatusUtil::str(status));
    }
    SubtreeRef const* ref = subtree_cast(block->get_data());
    id_ = ref->id;
    fanout_index_ = ref->fanout_index;
    prev_ = ref->addr;
    write_pos_ = ref->payload_size;
    level_ = ref->level;
}

aku_Status NBTreeSuperblock::append(SubtreeRefPayload const& p) {
    if (is_full()) {
        return AKU_EOVERFLOW;
    }
    if (immutable_) {
        return AKU_EBAD_DATA;
    }
    SubtreeRef ref;
    ref.version = AKUMULI_VERSION;
    ref.level = 1;  // because leaf node was added
    ref.payload_size = 0;  // Not used in supernodes

    ref.addr = p.addr;
    ref.max = p.max;
    ref.min = p.min;
    ref.sum = p.sum;
    ref.begin = p.begin;
    ref.end = p.end;
    ref.id = p.id;
    ref.count = p.count;

    // Write data into buffer
    SubtreeRef* pref = subtree_cast(buffer_.data());
    pref += (1 + write_pos_);
    *pref = ref;
    write_pos_++;
    return AKU_SUCCESS;
}

std::tuple<aku_Status, LogicAddr> NBTreeSuperblock::commit(std::shared_ptr<BlockStore> bstore) {
    if (immutable_) {
        return std::make_tuple(AKU_EBAD_DATA, EMPTY);
    }
    if (fanout_index_ != 0) {
        SubtreeRef* backref = subtree_cast(buffer_.data());
        NBTreeSuperblock subtree(prev_, bstore);
        aku_Status status = init_subtree_from_subtree(subtree, *backref);
        if (status != AKU_SUCCESS) {
            return std::make_tuple(status, EMPTY);
        }
        backref->payload_size = write_pos_;
        backref->fanout_index = fanout_index_;
        backref->id = id_;
        backref->level = level_;
        backref->version = AKUMULI_VERSION;
    }
    return bstore->append_block(buffer_.data());
}

bool NBTreeSuperblock::is_full() const {
    static const size_t maxsize = AKU_BLOCK_SIZE / sizeof(SubtreeRef) - 1;
    return write_pos_ < maxsize;
}

aku_Status NBTreeSuperblock::read_all(std::vector<SubtreeRef>* refs) const {
    for(u32 ix = 0u; ix < write_pos_; ix++) {
        SubtreeRef const* ref = subtree_cast(buffer_.data());
        ref += (1 + ix);
        refs->push_back(*ref);
    }
    return AKU_SUCCESS;
}


// //////////////////////// //
//        NBTreeRoot        //
// //////////////////////// //


struct NBTreeRoot {
    virtual ~NBTreeRoot() = default;
    //! Append new data to the root (doesn't work with superblocks)
    virtual void append(aku_Timestamp ts, double value) = 0;
    //! Append subtree metadata to the root (doesn't work with leaf nodes)
    virtual void append(SubtreeRefPayload const& pl) = 0;
    //! Write all changes to the block-store, even if node is not full.
    virtual void commit() = 0;
};


struct NBTreeLeafRoot : NBTreeRoot {
    std::shared_ptr<BlockStore> bstore_;
    std::weak_ptr<NBTreeRootsCollection> roots_;
    aku_ParamId id_;
    LogicAddr last_;
    std::unique_ptr<NBTreeLeaf> leaf_;
    u16 fanout_index_;

    NBTreeLeafRoot(std::shared_ptr<BlockStore> bstore,
                   std::shared_ptr<NBTreeRootsCollection> roots,
                   aku_ParamId id,
                   LogicAddr last)
        : bstore_(bstore)
        , roots_(roots)
        , id_(id)
        , last_(last)
        , fanout_index_(0)
    {
        if (last_ != EMPTY) {
            // Load previous node and calculate fanout.
            aku_Status status;
            std::shared_ptr<Block> block;
            std::tie(status, block) = bstore_->read_block(last_);
            if (status != AKU_SUCCESS) {
                AKU_PANIC("Invalid argument, " + StatusUtil::str(status));
            }
            auto psubtree = subtree_cast(block->get_data());
            fanout_index_ = psubtree->fanout_index + 1;
            if (fanout_index_ == AKU_NBTREE_FANOUT) {
                fanout_index_ = 0;
                last_ = EMPTY;
            }
        }
        reset_leaf();
    }

    virtual void append(aku_Timestamp ts, double value) {
        // Invariant: leaf_ should be initialized, if leaf_ is full
        // and pushed to block-store, reset_leaf should be called
        aku_Status status = leaf_->append(ts, value);
        if (status == AKU_EOVERFLOW) {
            // Commit full node
            commit();
            // There should be only one level of recursion, no looping.
            // Stack overflow here means that there is a logic error in
            // the program that results in NBTreeLeaf::append always
            // returning AKU_EOVERFLOW.
            append(ts, value);
        }
        if (status != AKU_SUCCESS) {
            // it should return only AKU_EOVERFLOW
            AKU_PANIC("Unexpected error from NBTreeLeaf, " + StatusUtil::str(status));
        }
    }

    void reset_leaf() {
        leaf_.reset(new NBTreeLeaf(id_, last_, fanout_index_));
    }

    //! Forcibly commit changes, even if current page is not full
    virtual void commit() {
        // Invariant: after call to this method data from `leaf_` should
        // endup in block store, upper level root node should be updated
        // and `leaf_` variable should be reset.
        // Otherwise: panic should be triggered.

        LogicAddr addr;
        aku_Status status;
        std::tie(status, addr) = leaf_->commit(bstore_);
        if (status != AKU_SUCCESS) {
            AKU_PANIC("Can't write leaf-node to block-store, " + StatusUtil::str(status));
        }
        // Gather stats and send them to upper-level node
        SubtreeRefPayload payload;
        status = init_subtree_from_leaf(*leaf_, payload);
        if (status != AKU_SUCCESS) {
            // This shouldn't happen because leaf node can't be
            // empty just after overflow.
            AKU_PANIC("Can summarize leaf-node - " + StatusUtil::str(status));
        }
        payload.addr = addr;
        payload.id = id_;
        auto roots_collection = roots_.lock();
        if (roots_collection) {
            auto root = roots_collection->lease(1);
            root->append(payload);

            BOOST_SCOPE_EXIT_ALL(&) { roots_collection->release(std::move(root)); };

        } else {
            // Invariant broken.
            // Roots collection was destroyed before write process
            // stops.
            AKU_PANIC("Roots collection destroyed");
        }
        fanout_index_++;
        if (fanout_index_ == AKU_NBTREE_FANOUT) {
            fanout_index_ = 0;
            last_ = EMPTY;
        }
        last_ = addr;
        reset_leaf();
    }
};


struct NBSuperblockRoot : NBTreeRoot {
    std::shared_ptr<BlockStore> bstore_;
    std::weak_ptr<NBTreeRootsCollection> roots_;
    std::unique_ptr<NBTreeSuperblock> curr_;
    aku_ParamId id_;
    LogicAddr last_;
    u16 fanout_index_;
    u16 level_;

    NBSuperblockRoot(std::shared_ptr<BlockStore> bstore,
                     std::shared_ptr<NBTreeRootsCollection> roots,
                     aku_ParamId id,
                     LogicAddr last,
                     u16 level)
        : bstore_(bstore)
        , roots_(roots)
        , id_(id)
        , last_(last)
        , fanout_index_(0)
        , level_(level)
    {
        if (last_ != EMPTY) {
            aku_Status status;
            std::shared_ptr<Block> block;
            std::tie(status, block) = bstore_->read_block(last_);
            if (status != AKU_SUCCESS) {
                AKU_PANIC("Invalid argument, " + StatusUtil::str(status));
            }
            auto psubtree = subtree_cast(block->get_data());
            fanout_index_ = psubtree->fanout_index + 1;
            if (fanout_index_ == AKU_NBTREE_FANOUT) {
                fanout_index_ = 0;
                last_ = EMPTY;
            }
        }
        reset_subtree();
    }

    void reset_subtree() {
        curr_.reset(new NBTreeSuperblock(id_, last_, fanout_index_, level_));
    }

    virtual void append(aku_Timestamp ts, double value) {
        AKU_PANIC("Data should be added to the root 0");
    }

    virtual void append(SubtreeRefPayload const& pl) {
        auto status = curr_->append(pl);
        if (status == AKU_EOVERFLOW) {
            commit();
            append(pl);
        }
        if (status != AKU_SUCCESS) {
            AKU_PANIC("Can't append payload to superblock " + StatusUtil::str(status));
        }
    }

    virtual void commit() {
        // Invariant: after call to this method data from `curr_` should
        // endup in block store, upper level root node should be updated
        // and `curr_` variable should be reset.
        // Otherwise: panic should be triggered.

        LogicAddr addr;
        aku_Status status;
        std::tie(status, addr) = curr_->commit(bstore_);
        if (status != AKU_SUCCESS) {
            AKU_PANIC("Can't write leaf-node to block-store, " + StatusUtil::str(status));
        }
        // Gather stats and send them to upper-level node
        SubtreeRefPayload payload;
        status = init_subtree_from_subtree(*curr_, payload);
        if (status != AKU_SUCCESS) {
            AKU_PANIC("Can summarize current node - " + StatusUtil::str(status));
        }
        payload.addr = addr;
        payload.id = id_;
        auto roots_collection = roots_.lock();
        if (roots_collection) {
            auto root = roots_collection->lease(level_ + 1);
            root->append(payload);

            BOOST_SCOPE_EXIT_ALL(&) { roots_collection->release(std::move(root)); };

        } else {
            // Invariant broken.
            // Roots collection was destroyed before write process
            // stops.
            AKU_PANIC("Roots collection destroyed");
        }
        fanout_index_++;
        if (fanout_index_ == AKU_NBTREE_FANOUT) {
            fanout_index_ = 0;
            last_ = EMPTY;
        }
        last_ = addr;
        reset_subtree();
    }
};


// //////////////////////// //
//          NBTree          //
// //////////////////////// //


NBTree::NBTree(aku_ParamId id, std::shared_ptr<BlockStore> bstore)
    : bstore_(bstore)
    , id_(id)
    , last_(EMPTY)
{
    reset_leaf();
}

void NBTree::reset_leaf() {
    // TODO: this should be replaced with NBTreeRoot's logic
    leaf_.reset(new NBTreeLeaf(id_, last_, 0));
}

void NBTree::append(aku_Timestamp ts, double value) {
    // Invariant: leaf_ should be initialized, if leaf_ is full and pushed to block-store, reset_leaf should be called
    aku_Status status = leaf_->append(ts, value);
    if (status == AKU_EOVERFLOW) {
        LogicAddr addr;
        std::tie(status, addr) = leaf_->commit(bstore_);
        if (status != AKU_SUCCESS) {
            AKU_PANIC("Can't append data to the NBTree instance, " + StatusUtil::str(status));
        }
        last_ = addr;
        reset_leaf();
        // There should be only one level of recursion, no looping.
        // Stack overflow here means that there is a logic error in
        // the program that results in NBTreeLeaf::append always
        // returning AKU_EOVERFLOW.
        append(ts, value);
    }
    if (status != AKU_SUCCESS) {
        // it should return only AKU_EOVERFLOW
        AKU_PANIC("Unexpected error from NBTreeLeaf, " + StatusUtil::str(status));
    }
}

std::vector<LogicAddr> NBTree::roots() const {
    // NOTE: at this development stage implementation tracks only leaf nodes.
    // This is enough to test performance and build server and ingestion.
    std::vector<LogicAddr> rv = { last_ };
    return rv;
}

aku_ParamId NBTree::get_id() const {
    return id_;
}

aku_Status NBTree::read_all(std::vector<aku_Timestamp>* timestamps, std::vector<double>* values) const {
    return leaf_->read_all(timestamps, values);
}

std::vector<LogicAddr> NBTree::iter(aku_Timestamp start, aku_Timestamp stop) const {
    // Traverse tree from largest timestamp to smallest
    aku_Timestamp min = std::min(start, stop);
    aku_Timestamp max = std::max(start, stop);
    LogicAddr addr = last_;
    std::vector<LogicAddr> addresses;
    // Stop when EMPTY is hit or cycle detected.
    while (bstore_->exists(addr)) {
        std::unique_ptr<NBTreeLeaf> leaf;
        leaf.reset(new NBTreeLeaf(bstore_, addr, NBTreeLeaf::LeafLoadMethod::ONLY_HEADER));
        aku_Timestamp begin, end;
        std::tie(begin, end) = leaf->get_timestamps();
        if (min > end || max < begin) {
            addr = leaf->get_prev_addr();
            continue;
        }
        // Save address of the current leaf and move to the next one.
        addresses.push_back(addr);
        addr = leaf->get_prev_addr();
    }
    return addresses;
}

std::unique_ptr<NBTreeLeaf> NBTree::load(LogicAddr addr) const {
    std::unique_ptr<NBTreeLeaf> leaf;
    leaf.reset(new NBTreeLeaf(bstore_, addr));
    return std::move(leaf);
}

NBTreeCursor::NBTreeCursor(NBTree const& tree, aku_Timestamp start, aku_Timestamp stop)
    : tree_(tree)
    , start_(start)
    , stop_(stop)
    , eof_(false)
    , proceed_calls_(0)
    , id_(tree.get_id())
{
    ts_.reserve(SPACE_RESERVE);
    value_.reserve(SPACE_RESERVE);
    auto addrlist = tree.iter(start, stop);
    if (start > stop) {
        // Forward direction. Method tree.iter always return path in
        // backward direction so we need to reverse it.
        std::reverse(addrlist.begin(), addrlist.end());
    }
    std::swap(addrlist, backpath_);
    proceed();
}

aku_Status NBTreeCursor::load_next_page() {
    if (backpath_.empty()) {
        return AKU_ENO_DATA;
    }
    LogicAddr addr = backpath_.back();
    backpath_.pop_back();
    std::unique_ptr<NBTreeLeaf> leaf = tree_.load(addr);
    ts_.clear();
    value_.clear();
    return leaf->read_all(&ts_, &value_);
}

//! Returns number of elements in cursor
size_t NBTreeCursor::size() {
    return ts_.size();
}

//! Return true if read operation is completed and elements stored in this cursor
//! are the last ones.
bool NBTreeCursor::is_eof() {
    return eof_;
}

//! Read element from cursor (not all elements can be loaded to cursor)
std::tuple<aku_Status, aku_Timestamp, double> NBTreeCursor::at(size_t ix) {
    if (ix < ts_.size()) {
        return std::make_tuple(AKU_SUCCESS, ts_[ix], value_[ix]);
    }
    return std::make_tuple(AKU_EBAD_ARG, 0, 0);  // Index out of range
}

void NBTreeCursor::proceed() {
    aku_Status status = AKU_SUCCESS;
    if (start_ > stop_) {
        // Forward direction
        if (proceed_calls_ == 0) {
            // First call to proceed, need to push data from
            // tree_ first.
            ts_.clear();
            value_.clear();
            status = tree_.read_all(&ts_, &value_);
            // Ignore ENO_DATA error
            if (status != AKU_ENO_DATA && status != AKU_SUCCESS) {
                AKU_PANIC("Page load error - " + StatusUtil::str(status));
            }
            proceed_calls_++;
            return;
        }
    } else {
        // If backpath_ is empty we need to read data from tree_.
        if (backpath_.empty() && proceed_calls_ >= 0) {
            ts_.clear();
            value_.clear();
            status = tree_.read_all(&ts_, &value_);
            if (status != AKU_ENO_DATA && status != AKU_SUCCESS) {
                AKU_PANIC("Page load error - " + StatusUtil::str(status));
            }
            proceed_calls_ = -1;
            return;
        } else if (backpath_.empty() && proceed_calls_ < 0) {
            eof_ = true;
            return;
        }
    }
    status = load_next_page();
    if (status == AKU_ENO_DATA) {
        eof_ = true;
    } else if (status != AKU_SUCCESS) {
        AKU_PANIC("Page load error - " + StatusUtil::str(status));
    }
    proceed_calls_++;
}

}}
