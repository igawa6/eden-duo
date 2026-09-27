// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Manifest types, part 1: data points. Where a published value comes from and how it is
// derived: value types, pointer chains and their hops, the shape/pattern/text finders that
// locate a chain's start, spy points, derived values, and PointGate (a visibility test on a
// published value, used by the map and page types).
// Parsed in mod_manifest.cpp; resolved and sampled in mod_state.cpp.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Core::Mods {

enum class ValueType {
    U8,
    S8,
    U16,
    S16,
    U32,
    S32,
    U64,
    S64,
    F32,
    Bool,
    /// A managed string: length at +0x10, UTF-16 characters from +0x14. Reading these is what
    /// lets a package say "whatever the game is pointing at now" instead of hardcoding.
    Utf16String,
    Utf32String,
    /// A plain zero-terminated C string.
    CString,
};

enum class BaseRegion {
    Main,     ///< relative to the main NSO load base
    Absolute, ///< raw guest virtual address
};

/// An address found by searching the main module for a byte pattern, rather than being written
/// down. Wildcards ("??") cover bytes that move between builds, which is how a package can survive
/// a game update -- and the only way to locate anything in a game that ships no metadata.
struct PatternFind {
    std::vector<u8> bytes;
    std::vector<u8> mask; ///< 1 = must match, 0 = wildcard
    s64 offset{0};        ///< added to the match address
    u32 index{0};         ///< which match to take, when a pattern is not unique
    /// Search the heap rather than the module. A signature made of live data -- the bytes that
    /// happen to surround a value the game is using -- is not in the executable at all.
    bool heap{false};
    [[nodiscard]] bool Valid() const {
        return !bytes.empty() && bytes.size() == mask.size();
    }
};

/// A visibility test on a published value: open while it reads non-zero (ints first, then floats;
/// missing = 0), or while it reads zero when `negate` ("!name" in JSON).
struct PointGate {
    std::string point;
    bool negate{false};
    [[nodiscard]] bool Empty() const {
        return point.empty();
    }
};

/// Watch a function the game calls itself and keep an argument from it. Some things cannot be
/// read from static memory -- a scripting VM's state is only ever handed around in registers --
/// so the way to obtain one is to look over the game's shoulder when it makes the call.
struct SpyPoint {
    std::string name;
    std::string symbol; ///< "$Name" resolved from the data file's symbols
    u32 reg{0};         ///< which argument register to keep
};

/// One hop along a pointer chain. Beyond a fixed offset it can index an array -- which is how you
/// reach a child transform, a component slot, or an inventory entry -- and the index itself can
/// come from another data point, so it follows the game's own selection.
struct ChainHop {
    s64 offset{0};
    s64 stride{0}; ///< element size; 0 means no indexing
    s64 index{0};
    std::string index_bind;    ///< when set, the index is read from this point each time
    bool static_fields{false}; ///< resolve IL2CPP's static block from the class instead
    /// Walk an intrusive circular list (sead::OffsetList and its like) instead of indexing an
    /// array. `list_next` is where a node keeps the pointer to the next one; `list_node_at` is
    /// where the list head keeps the distance from a node back to the object around it. The hop
    /// does not dereference first: the list head sits at `address + offset`.
    bool is_list{false};
    s64 list_next{0};
    s64 list_node_at{0};
    /// This hop's index comes from the array element being sampled rather than from the manifest,
    /// which is what lets one declaration stand for a whole inventory.
    bool index_from_array{false};
};

/// Locate the player's transform by shape, then by behaviour.
///
/// Dread has no class that means "the player": position lives in a generic scene-graph node
/// that every one of thirty thousand actors also has, and the player hangs too deep off that
/// tree for any short route from a module static to reach it. What is unique is not the shape
/// but the conduct -- only the player starts and stops with the stick. So: take every node of
/// the class holding world-scale coordinates, then watch which one agrees with the controller.
struct PlayerFind {
    s64 vtable{0};          ///< scene-graph node class, relative to the module
    s64 pos_offset{0};      ///< where the x/y/z floats sit inside that node
    int min_samples{90};    ///< samples to take before trusting a winner
    float min_abs{1000.0f}; ///< smallest plausible world coordinate: a scene graph is mostly
                            ///< local transforms, and those sit near their parent, not the
                            ///< world origin. Without this the search wins on a child node
                            ///< that tracks the stick perfectly and reports the wrong place.
    [[nodiscard]] bool Valid() const {
        return vtable != 0;
    }
};

/// An array of uniform entries, found by its own shape rather than by an address.
///
/// Dread keeps each item's amount in a {vtable, tag, float} entry, packed at a 0x18 stride --
/// which is why no two amounts were ever neighbours, and why every search for one value beside
/// another failed. The entries have no key in them, but the run itself is unmistakable: nothing
/// else in the heap repeats a known vtable a dozen times at a fixed interval. That makes the
/// array locatable in any run without a pointer chain, and an item is then simply an index.
struct ArrayFind {
    s64 vtable{0};  ///< module offset of the value the entry starts with
    s64 stride{0};  ///< bytes per entry
    s64 index{0};   ///< which entry
    int min_run{8}; ///< how many consecutive entries make it the array and not a coincidence
    [[nodiscard]] bool Valid() const {
        return vtable != 0 && stride > 0;
    }
};

/// Locate a point by the words the game itself wrote there. Some UI state lives in buffers that
/// nothing points at; the text in them is the only stable handle we get.
struct TextScan {
    std::vector<std::string> candidates; ///< any of these marks the buffer
    s64 offset{0};                       ///< added to the match address
    [[nodiscard]] bool Valid() const {
        return !candidates.empty();
    }
};

/// One readable value: base + chain of pointer hops + final offset.
struct DataPoint {
    ValueType type{ValueType::S32};
    ArrayFind array;            ///< when set, the chain starts at an entry of a shape-located array
    PlayerFind player;          ///< when set, the chain starts at the player's transform node
    PatternFind find;           ///< when set, replaces the chain's first hop
    TextScan text_scan;         ///< when set, the chain starts at a buffer found by its own text
    std::string class_name;     ///< when set, the chain starts at this class' TypeInfo slot
    std::vector<ChainHop> hops; ///< richer form of `chain`, used when non-empty
    /// When set, the chain starts at an address a call sequence published rather than at the
    /// module base -- the object the game just handed us, wherever it happens to live.
    std::string root_bind;
    BaseRegion base{BaseRegion::Main};
    std::vector<s64> chain; ///< chain[0] is added to the base; each later hop dereferences first
    s64 offset{0};
    bool is_pointer{false}; ///< expose the resolved address rather than the value
    /// Sample this point `count` times, once per element, and publish the results as
    /// "<name>0", "<name>1", ... A list of twenty items is one declaration, not twenty.
    s64 count{1};
    std::string count_bind; ///< when set, how many elements to read is itself read from memory
    /// Integer post-read modifiers, applied in this order after the read (per element for a
    /// `count` array; ignored for f32, strings and `pointer` points): the value is first taken
    /// as the unsigned bit pattern of its type width, then `>> shift`, then `& mask`, then
    /// popcount. "bit": n is shift n + mask 1. A write to a shift/mask point only replaces those
    /// bits; a popcount point cannot be written.
    s32 shift{0};
    bool has_mask{false};
    u64 mask{~u64{0}};
    bool popcount{false};
    /// Element step of a `count` array for a chain without an "index": "$i" hop:
    /// element i sits at address + i * stride.
    s64 stride{0};
    [[nodiscard]] bool HasModifiers() const {
        return shift != 0 || has_mask || popcount;
    }
};

/// A value computed from other published values after every sample: sum(source * factor) + add,
/// optionally floored or rounded, published like a point (ints and floats). A term whose source is
/// missing makes the result missing. The hold form publishes `hold_last_nonzero` while the gate
/// (`hold_gate`, default the source itself) reads non-zero, else the last value published while it
/// did -- a position the game zeroes during level loads.
struct DerivedPoint {
    std::string name;
    std::vector<std::pair<std::string, f64>> terms;
    f64 add{0.0};
    bool floor{false};
    bool round{false};
    std::string hold_last_nonzero;
    std::string hold_gate;
    /// Select form: publishes `select_then` while `select` reads non-zero, else `select_else`
    /// (a missing condition or chosen source makes the result missing).
    std::string select;
    std::string select_then;
    std::string select_else;
    /// any_eq form: the number of elements "<array>0".."<array>N-1" equal to the value
    /// (missing when any element is missing).
    std::string any_eq_array;
    s64 any_eq_count{0};
    s64 any_eq_value{0};
    /// One side of a `cmp` comparison -- either a literal constant (a JSON number)
    /// or a published name to look up (a point, another derived value, or "@flag:"/"@sel:"/etc,
    /// the same namespace every other derived source reads).
    struct CmpOperand {
        bool is_const{false};
        f64 const_value{0.0};
        std::string name; ///< used when !is_const; an empty, non-const operand is always missing
    };
    /// cmp form: publishes 1.0/0.0 for "cmp_a <cmp_op> cmp_b" ("eq"/"ne"/"ge"/"gt"/"le"/"lt"),
    /// missing when either side is missing. cmp_op empty = not a cmp entry.
    std::string cmp_op;
    CmpOperand cmp_a;
    CmpOperand cmp_b;
    /// all_nonzero / any_nonzero form: publishes 1.0/0.0 over a list of published names, missing
    /// when any one of them is missing (fails closed, like any_eq). Empty list = not this form.
    std::vector<std::string> nonzero_sources;
    bool nonzero_require_all{false}; ///< true = all_nonzero, false = any_nonzero
};

} // namespace Core::Mods
