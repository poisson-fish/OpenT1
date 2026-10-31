#pragma once
#include <opent1/core/persist.hpp>
#include <opent1/core/res_manager.hpp>
#include <array>

namespace opent1::sim {
class BayesNet;

class ChanceNode : public Persistent::VersionedBase {
public:
    ChanceNode() = default;
    ChanceNode(const ChanceNode&) = delete;
    ChanceNode& operator=(const ChanceNode&) = delete;

    struct State {
        std::string name;
        UInt32 valueIndex{}, flags{};
    };

    ~ChanceNode() override;
    static constexpr UInt32 ReadOnly = 1;

    [[nodiscard]] Int32 getId() const {
        return id_;
    }

    [[nodiscard]] std::string_view getName() const {
        return name_;
    }

    bool setName(std::string_view);
    bool addState(std::string_view);
    bool delState(std::size_t);
    bool setStateName(std::size_t, std::string_view);
    bool setReadOnly(std::size_t, bool);
    bool setState(std::size_t, Flt32);
    [[nodiscard]] std::optional<Flt32> getState(std::size_t) const;

    [[nodiscard]] std::span<const State> states() const {
        return states_;
    }

    [[nodiscard]] std::span<ChanceNode* const> parents() const {
        return parents_;
    }

    [[nodiscard]] std::span<ChanceNode* const> children() const {
        return children_;
    }

    [[nodiscard]] std::span<const Flt32> probabilities() const {
        return cpt_;
    }

    bool setCP(Flt32,
               std::span<const std::size_t> states); // own state, then parents in attachment order
    bool setCPVal(Flt32, std::size_t state, std::size_t parentCombination);
    [[nodiscard]] std::optional<Flt32> getCPVal(std::size_t state,
                                                std::size_t parentCombination) const;
    std::array<Int32, 4> rectangle{}; // legacy RectI coordinates, explicit scalar encoding
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    friend class BayesNet;
    friend bool registerBayesResource(ResourceManager&);
    BayesNet* net_{};
    Int32 id_{};
    std::string name_;
    std::vector<State> states_;
    std::vector<ChanceNode*> parents_, children_;
    std::vector<Int32> pendingParents_;
    std::vector<Flt32> cpt_;
};

using BayesNode = ChanceNode;

// Owns created nodes. A deserialized network borrows its nodes from LoadContext,
// which must outlive it. Graph editing needs external locking and invalidates
// existing instances. Independent evaluations never change network-global state.
class BayesNet : public Persistent::VersionedBase {
public:
    ~BayesNet() override;
    ChanceNode* addNode(std::string_view name);
    bool delNode(ChanceNode&);
    bool addParent(ChanceNode& child, ChanceNode& parent);
    bool delParent(ChanceNode& child, ChanceNode& parent);
    [[nodiscard]] ChanceNode* findNode(Int32) const;

    [[nodiscard]] std::span<ChanceNode* const> nodes() const {
        return nodes_;
    }

    [[nodiscard]] std::vector<ChanceNode*> topologicalOrder() const;

    [[nodiscard]] std::span<const Flt32> stateValues() const {
        return values_;
    }

    // Preserves legacy unnormalized sums and read-only dependent states.
    // Rejects nonfinite inputs, bad extents and an evaluation exceeding its budget.
    bool evaluate(std::span<Flt32>, std::size_t operationBudget = 16000000) const;
    bool evaluate();

    [[nodiscard]] UInt64 revision() const {
        return revision_;
    }

    void clear();
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    friend class ChanceNode;
    friend bool registerBayesResource(ResourceManager&);
    bool resizeStates(ChanceNode&, std::optional<std::size_t> erase, std::string_view added = {});
    void nodeDestroyed(ChanceNode&);
    std::vector<std::unique_ptr<ChanceNode>> owned_;
    std::vector<ChanceNode*> nodes_;
    std::vector<Flt32> values_;
    Int32 nextId_{10001};
    UInt64 revision_{};
    std::shared_ptr<void> loadedOwner_;
};

class BayesNetInstance {
public:
    explicit BayesNetInstance(std::shared_ptr<const BayesNet>);
    explicit BayesNetInstance(Resource<BayesNet>);
    // Callbacks retain their own external lifetimes. Empty callbacks unbind.
    bool registerStateVariable(std::string_view stateName,
                               std::function<Flt32()> get,
                               std::function<void(Flt32)> set);
    bool evaluateNetwork();

    [[nodiscard]] std::span<const Flt32> stateValues() const {
        return values_;
    }

private:
    struct Binding {
        std::size_t index;
        std::function<Flt32()> get;
        std::function<void(Flt32)> set;
    };

    const BayesNet* net_{};
    std::shared_ptr<const BayesNet> shared_;
    Resource<BayesNet> resource_;
    std::vector<Flt32> values_;
    std::vector<Binding> bindings_;
    UInt64 revision_{};
};

bool registerBayesClasses(Persistent::ClassRegistry&);
// Resource owns both the .ai root and all loaded nodes through its context.
bool registerBayesResource(ResourceManager&);
} // namespace opent1::sim
