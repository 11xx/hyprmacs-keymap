#include "PrefixTree.hpp"

namespace hyprmacs {

const Node* Node::child(const Chord& c) const {
    auto it = children.find({c.mods, c.sym, c.code});
    return it == children.end() ? nullptr : it->second.get();
}

InsertStatus PrefixTree::insert(const std::vector<Chord>& seq, const Action& action, bool strictDuplicates) {
    if (seq.empty())
        return InsertStatus::ErrEmpty;

    Node* node = &m_root;
    for (size_t i = 0; i < seq.size(); ++i) {
        const auto  key    = std::make_tuple(seq[i].mods, seq[i].sym, seq[i].code);
        const bool  isLast = (i + 1 == seq.size());
        auto&       slot   = node->children[key];

        if (!slot)
            slot = std::make_unique<Node>();
        Node* child = slot.get();

        if (!isLast) {
            // We are descending into a prefix position. If the existing node is
            // a final binding, this is a prefix-over-final conflict.
            if (child->isLeaf())
                return InsertStatus::ErrPrefixOverFinal;
            node = child;
            continue;
        }

        // Final position.
        if (child->isPrefix())
            return InsertStatus::ErrFinalOverPrefix;

        if (child->isLeaf()) {
            if (strictDuplicates)
                return InsertStatus::ErrStrictDuplicate;
            child->actions.push_back(action);
            return InsertStatus::AddedDuplicate;
        }

        child->actions.push_back(action);
        return InsertStatus::OK;
    }

    return InsertStatus::OK; // unreachable
}

} // namespace hyprmacs
