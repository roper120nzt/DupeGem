#ifndef BK_TREE_H
#define BK_TREE_H

#include <vector>
#include <unordered_map>
#include <functional>
#include <bitset>

template <typename HashT>
class BKTree {
public:
    using DistanceFunc = std::function<int(const HashT&, const HashT&)>;

    BKTree(DistanceFunc f) : dist(f), root(nullptr) {}

    ~BKTree() { deleteTree(root); }

    BKTree(const BKTree&) = delete;
    BKTree& operator=(const BKTree&) = delete;

    BKTree(BKTree&& other) noexcept : dist(std::move(other.dist)), root(other.root) {
        other.root = nullptr;
    }

    BKTree& operator=(BKTree&& other) noexcept {
        if (this != &other) {
            deleteTree(root);
            dist = std::move(other.dist);
            root = other.root;
            other.root = nullptr;
        }
        return *this;
    }

    void add(const HashT& value, int index) {
        if (!root) {
            root = new Node(value, index);
            return;
        }
        Node* curr = root;
        while (true) {
            int d = dist(value, curr->value);
            if (d == 0) {
                curr->indices.push_back(index);
                return;
            }
            auto it = curr->children.find(d);
            if (it == curr->children.end()) {
                curr->children[d] = new Node(value, index);
                return;
            }
            curr = it->second;
        }
    }

    std::vector<int> query(const HashT& value, int maxDist, int exclude = -1) const {
        std::vector<int> results;
        if (!root) return results;
        std::vector<Node*> stack = {root};
        while (!stack.empty()) {
            Node* node = stack.back();
            stack.pop_back();
            int d = dist(value, node->value);
            if (d <= maxDist) {
                for (int idx : node->indices) {
                    if (idx != exclude) {
                        results.push_back(idx);
                    }
                }
            }
            for (int i = std::max(0, d - maxDist); i <= d + maxDist; ++i) {
                auto it = node->children.find(i);
                if (it != node->children.end()) {
                    stack.push_back(it->second);
                }
            }
        }
        return results;
    }

private:
    struct Node {
        HashT value;
        std::vector<int> indices;
        std::unordered_map<int, Node*> children;
        Node(const HashT& v, int idx) : value(v), indices{idx} {}
    };

    static void deleteTree(Node* node) {
        // A degenerate tree can be tens of thousands of nodes deep. Recursive
        // destruction would overflow the process stack on large collections.
        if (!node) return;
        std::vector<Node*> stack{node};
        while (!stack.empty()) {
            Node* current = stack.back();
            stack.pop_back();
            for (auto& child : current->children) stack.push_back(child.second);
            delete current;
        }
    }

    DistanceFunc dist;
    Node* root;
};

#endif // BK_TREE_H
