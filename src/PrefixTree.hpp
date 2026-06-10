// -*- mode: c++; -*-
// Prefix tree of chord sequences. Each interior node is a prefix; each node
// holding one or more actions is a final ("leaf") binding. A node can never be
// both — that is a registration error, matching the Org behaviour.
#pragma once

#include "Core.hpp"

#include <map>
#include <memory>
#include <tuple>
#include <vector>

namespace hyprmacs {

struct Node {
    // children keyed by (mods, sym, code); std::map keeps this header dependency-free.
    std::map<std::tuple<Mods, Keysym, Keycode>, std::unique_ptr<Node>> children;
    // non-empty => this node is a final binding. Multiple entries model the
    // Org's "multiple commands on the same key" behaviour (run in order).
    std::vector<Action> actions;

    bool        isLeaf() const { return !actions.empty(); }
    bool        isPrefix() const { return !children.empty(); }
    const Node* child(const Chord& c) const;
};

enum class InsertStatus {
    OK,                  // inserted a brand-new final binding
    AddedDuplicate,      // appended to an existing final binding (default policy)
    ErrPrefixOverFinal,  // tried to register a prefix through an existing final
    ErrFinalOverPrefix,  // tried to register a final on an existing prefix
    ErrStrictDuplicate,  // duplicate final binding rejected by strict policy
    ErrEmpty,            // empty sequence
};

class PrefixTree {
  public:
    const Node* root() const { return &m_root; }

    // Insert a full chord sequence as a final binding for `action`.
    // `strictDuplicates` rejects duplicate finals instead of appending.
    InsertStatus insert(const std::vector<Chord>& seq, const Action& action, bool strictDuplicates);

    void clear() { m_root = Node{}; }

  private:
    Node m_root;
};

} // namespace hyprmacs
