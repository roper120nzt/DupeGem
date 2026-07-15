#ifndef BK_TREE_H
#define BK_TREE_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// A compact BK-tree specialized for fixed-size bit hashes. Nodes and child
// links live in contiguous vectors, eliminating one allocation and one hash
// table per distinct value. Once construction ends, concurrent queries are
// read-only and safe.
template <typename HashT>
class BKTree {
public:
    BKTree() = default;

    BKTree(const BKTree&) = delete;
    BKTree& operator=(const BKTree&) = delete;
    BKTree(BKTree&&) noexcept = default;
    BKTree& operator=(BKTree&&) noexcept = default;

    void reserve(size_t count) { nodes_.reserve(count); }
    size_t size() const { return nodes_.size(); }

    void add(const HashT& value, int index) {
        if (nodes_.empty()) {
            nodes_.push_back(Node{value, {index}, {}});
            return;
        }
        uint32_t current = 0;
        while (true) {
            const int distance = metric(value, nodes_[current].value);
            if (distance == 0) {
                nodes_[current].indices.push_back(index);
                return;
            }
            auto& children = nodes_[current].children;
            const auto found = std::lower_bound(
                children.begin(), children.end(), distance,
                [](const Child& child, int target) { return child.distance < target; });
            if (found != children.end() && found->distance == distance) {
                current = found->node;
                continue;
            }

            const size_t insertAt = size_t(found - children.begin());
            const uint32_t newNode = uint32_t(nodes_.size());
            nodes_.push_back(Node{value, {index}, {}});
            // nodes_.push_back may reallocate, so reacquire the child vector.
            auto& stableChildren = nodes_[current].children;
            stableChildren.insert(stableChildren.begin() + ptrdiff_t(insertAt),
                                  Child{uint16_t(distance), newNode});
            return;
        }
    }

    std::vector<int> query(const HashT& value, int maxDistance,
                           int exclude = -1) const {
        std::vector<int> results;
        if (nodes_.empty()) return results;
        std::vector<uint32_t> stack{0};
        while (!stack.empty()) {
            const uint32_t current = stack.back();
            stack.pop_back();
            const Node& node = nodes_[current];
            const int distance = metric(value, node.value);
            if (distance <= maxDistance) {
                for (int index : node.indices)
                    if (index != exclude) results.push_back(index);
            }
            const int minimum = std::max(0, distance - maxDistance);
            const int maximum = distance + maxDistance;
            auto child = std::lower_bound(
                node.children.begin(), node.children.end(), minimum,
                [](const Child& value, int target) { return value.distance < target; });
            for (; child != node.children.end() && child->distance <= maximum; ++child)
                stack.push_back(child->node);
        }
        return results;
    }

private:
    struct Child {
        uint16_t distance{};
        uint32_t node{};
    };
    struct Node {
        HashT value;
        std::vector<int> indices;
        std::vector<Child> children;
    };

    static int metric(const HashT& left, const HashT& right) {
        return int((left ^ right).count());
    }

    std::vector<Node> nodes_;
};

#endif // BK_TREE_H
