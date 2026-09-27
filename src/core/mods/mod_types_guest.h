// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Manifest types, part 6: guest plumbing. Guest call sequences, code patches written at
// startup, enforce rules, and the IL2CPP class layout. Run by mod_guest_bridge.cpp and
// mod_state.cpp; the IL2CPP layout is read by engine_il2cpp.cpp.

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core::Mods {

/// One call in a sequence run on a single borrowed guest thread. A scripting VM has no entry
/// point that answers a question outright: you push the table, push the function, push the
/// arguments, call, read the result, and put the stack back. Each step has to see the one before
/// it, which a single fire-and-forget call cannot do.
struct CallStep {
    std::string fn;                ///< symbol naming the guest function
    std::vector<std::string> args; ///< "$L", "$ret", "$#slot", "$@Symbol", "$point", or a number
    bool ret_float{false};         ///< result arrives in s0 -- Dread built Lua with float numbers
    std::string save;              ///< keep this step's result under a name later steps can read
    std::string out;               ///< publish this step's result as a snapshot number
    /// The result is a pointer: publish it as an address, which a data point can then hang a
    /// pointer chain off. This is how a value the VM will not hand over -- a world position --
    /// becomes readable: ask Lua for the object, then walk the object in memory.
    std::string out_addr;
    std::string out_text; ///< the result is a char*; publish the string it points at
    /// Guard. A scripting VM raises errors by longjmp, which on a thread we have borrowed would
    /// unwind straight past our return trampoline and leave the game holding a broken stack. So
    /// a step can check what it got -- typically a type -- and jump to the cleanup step instead
    /// of walking into an index-a-nil.
    bool has_expect{false};
    s64 expect{0};
    s64 else_step{-1};
    /// The mirror of `expect`: leave if the result *is* this. A game can run several scripting
    /// states at once -- one driving the interface, another the world -- and a hook lands in
    /// whichever happens to call. Asking a cheap question first and giving up when the answer is
    /// the empty one keeps a sequence from publishing a menu's idea of the player's inventory.
    bool has_reject{false};
    s64 reject{0};
};

/// A named sequence of steps, and what to do with what it returns.
struct CallSequence {
    std::vector<CallStep> steps;
    std::string out; ///< publish the final result as this snapshot value
    bool out_float{false};
    u32 every_ms{0};  ///< 0 = only when an action fires it; otherwise polled
    std::string flag; ///< only run while this runtime flag is set
};

/// An instruction (or a few) written over the game's own code once, at startup. Emulation bugs
/// sometimes hand a game a null where it never checks for one, and the game then dereferences it;
/// a package can carry the guard the code is missing rather than waiting for the emulator to grow
/// one. Kept in the per-build data file, since an address means nothing across builds.
struct GuestPatch {
    s64 at{};               ///< offset from the main module base
    std::vector<u32> words; ///< instructions to write, in order
    std::string why;        ///< recorded so a patch is never a mystery later
    /// Patches that repair a crash seen under one CPU backend can cause one under another: a
    /// device that runs guest code close to the metal may not see the patched instructions at
    /// all, and half-applied code is worse than none. Optional patches stay off unless a run
    /// asks for them (EDEN_DSMOD_PATCHES=1), so a package is safe on hardware it was not tested
    /// on.
    bool optional{false};
};

/// A rule the runtime applies on its own, without a tap. Games undo things we do to them --
/// Hollow Knight's FSMs switch the HUD back on at every scene transition -- so state we impose
/// has to be re-imposed periodically rather than set once.
struct EnforceRule {
    std::string action; ///< action to run
    u32 every_ms{500};  ///< how often
    std::string flag;   ///< only when this runtime flag has the value below (empty = always)
    bool flag_value{true};
};

/// Where the fields we walk sit inside IL2CPP's own Il2CppClass. This moves between Unity
/// versions -- Hollow Knight's 1.3 build is metadata v23 and its 1.5 update is v31 -- so the
/// per-build data file carries it, defaulting to the v23 layout.
struct Il2CppLayout {
    s64 name{0x10};
    s64 methods{0x80};
    s64 method_count{0xF4};
};

} // namespace Core::Mods
