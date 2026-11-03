#include <opent1/sim/bayes_ai.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace opent1::sim {
namespace {
constexpr std::size_t MaxTable = 1048576, MaxTotalTable = 16777216, MaxNodes = 4096,
                      MaxValues = 65536;

std::size_t totalTable(std::span<ChanceNode* const> nodes) {
    std::size_t total{};
    std::ranges::for_each(nodes, [&](const auto* node) {
        total += node->probabilities().size();
    });
    return total;
}

bool validName(std::string_view name) {
    return name.size() <= 255 && name.find('\0') == std::string_view::npos;
}

std::size_t tableSize(std::size_t states,
                      std::span<ChanceNode* const> parents,
                      const ChanceNode* changed = nullptr,
                      std::size_t count = 0) {
    auto total = states;
    for (const auto* parent : parents) {
        const auto size = parent == changed ? count : parent->states().size();
        if (size && total > MaxTable / size) {
            return MaxTable + 1;
        }
        total *= size;
    }
    return total;
}

bool equalName(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
        const auto fold = [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? c + 32 : c;
        };
        return fold(x) == fold(y);
    });
}

bool writeName(StreamIO& stream, std::string_view name) {
    return stream.write(static_cast<Int32>(name.size() + 1)) &&
           stream.writeBytes(std::as_bytes(std::span(name))) && stream.write(UInt8{0});
}

bool readName(StreamIO& stream, std::string& name) {
    Int32 length{};
    if (!stream.read(length) || length < 1 || length > 256) {
        return false;
    }
    std::string value(static_cast<std::size_t>(length), '\0');
    if (!stream.readBytes(std::as_writable_bytes(std::span(value))) || value.back() != '\0') {
        return false;
    }
    value.pop_back();
    if (!validName(value)) {
        return false;
    }
    name = std::move(value);
    return true;
}
}

ChanceNode::~ChanceNode() {
    if (net_) {
        net_->nodeDestroyed(*this);
    }
}

bool ChanceNode::setName(std::string_view name) {
    if (!validName(name)) {
        return false;
    }
    name_ = name;
    return true;
}

bool ChanceNode::addState(std::string_view name) {
    return net_ && validName(name) && net_->resizeStates(*this, {}, name);
}

bool ChanceNode::delState(std::size_t index) {
    return net_ && index < states_.size() && net_->resizeStates(*this, index);
}

bool ChanceNode::setStateName(std::size_t index, std::string_view name) {
    if (index >= states_.size() || !validName(name)) {
        return false;
    }
    states_[index].name = name;
    if (net_) {
        ++net_->revision_;
    }
    return true;
}

bool ChanceNode::setReadOnly(std::size_t index, bool value) {
    if (index >= states_.size()) {
        return false;
    }
    states_[index].flags = value ? ReadOnly : 0;
    return true;
}

bool ChanceNode::setState(std::size_t index, Flt32 value) {
    if (!net_ || index >= states_.size() || !std::isfinite(value)) {
        return false;
    }
    net_->values_[states_[index].valueIndex] = value;
    return true;
}

std::optional<Flt32> ChanceNode::getState(std::size_t index) const {
    return net_ && index < states_.size() ? std::optional(net_->values_[states_[index].valueIndex])
                                          : std::nullopt;
}

bool ChanceNode::setCP(Flt32 value, std::span<const std::size_t> indices) {
    if (parents_.empty() || indices.size() != parents_.size() + 1 || indices[0] >= states_.size()) {
        return false;
    }
    std::size_t index = indices[0];
    for (auto i : std::views::iota(std::size_t{0}, parents_.size())) {
        const auto size = parents_[i]->states_.size();
        if (indices[i + 1] >= size) {
            return false;
        }
        index = index * size + indices[i + 1];
    }
    if (index >= cpt_.size() || !std::isfinite(value)) {
        return false;
    }
    cpt_[index] = value;
    return true;
}

bool ChanceNode::setCPVal(Flt32 value, std::size_t state, std::size_t item) {
    const auto count = tableSize(1, parents_);
    if (parents_.empty() || state >= states_.size() || item >= count || !std::isfinite(value)) {
        return false;
    }
    cpt_[state * count + item] = value;
    return true;
}

std::optional<Flt32> ChanceNode::getCPVal(std::size_t state, std::size_t item) const {
    const auto count = tableSize(1, parents_);
    return !parents_.empty() && state < states_.size() && item < count
               ? std::optional(cpt_[state * count + item])
               : std::nullopt;
}

bool BayesNet::resizeStates(ChanceNode& node,
                            std::optional<std::size_t> erase,
                            std::string_view name) {
    if ((!erase && values_.size() >= MaxValues) || node.net_ != this) {
        return false;
    }
    const auto count = erase ? node.states_.size() - 1 : node.states_.size() + 1;
    std::vector<std::pair<ChanceNode*, std::vector<Flt32>>> replacements;
    const auto ownSize = tableSize(count, node.parents_);
    if (ownSize > MaxTable) {
        return false;
    }
    auto total = totalTable(nodes_) - node.cpt_.size() + ownSize;
    replacements.emplace_back(&node, std::vector<Flt32>(ownSize, 1));
    for (auto* child : node.children_) {
        const auto size = tableSize(child->states_.size(), child->parents_, &node, count);
        total = total - child->cpt_.size() + size;
        if (size > MaxTable || total > MaxTotalTable) {
            return false;
        }
        replacements.emplace_back(child, std::vector<Flt32>(size, 1));
    }
    if (total > MaxTotalTable) {
        return false;
    }
    if (erase) {
        node.states_.erase(node.states_.begin() + static_cast<std::ptrdiff_t>(*erase));
    } else {
        ChanceNode::State state{std::string(name), static_cast<UInt32>(values_.size()), 0};
        node.states_.reserve(count);
        values_.reserve(values_.size() + 1);
        node.states_.push_back(std::move(state));
        values_.push_back(0);
    }
    std::ranges::for_each(replacements, [](auto& replacement) {
        replacement.first->cpt_ = std::move(replacement.second);
    });
    ++revision_;
    return true;
}

BayesNet::~BayesNet() {
    clear();
}

void BayesNet::clear() {
    std::ranges::for_each(nodes_, [](auto* node) {
        node->net_ = nullptr;
        node->parents_.clear();
        node->children_.clear();
    });
    nodes_.clear();
    owned_.clear();
    values_.clear();
    nextId_ = 10001;
    ++revision_;
}

void BayesNet::nodeDestroyed(ChanceNode& node) {
    std::ranges::for_each(node.parents_, [&](auto* parent) {
        std::erase(parent->children_, &node);
    });
    std::ranges::for_each(node.children_, [&](auto* child) {
        std::erase(child->parents_, &node);
    });
    std::erase(nodes_, &node);
    ++revision_;
}

ChanceNode* BayesNet::addNode(std::string_view name) {
    if (!validName(name) || nodes_.size() >= MaxNodes ||
        nextId_ == std::numeric_limits<Int32>::max()) {
        return nullptr;
    }
    auto node = std::make_unique<ChanceNode>();
    node->name_ = name;
    node->id_ = nextId_;
    node->net_ = this;
    nodes_.reserve(nodes_.size() + 1);
    owned_.reserve(owned_.size() + 1);
    auto* result = node.get();
    owned_.push_back(std::move(node));
    nodes_.push_back(result);
    ++nextId_;
    ++revision_;
    return result;
}

ChanceNode* BayesNet::findNode(Int32 id) const {
    const auto it = std::ranges::find(nodes_, id, &ChanceNode::getId);
    return it == nodes_.end() ? nullptr : *it;
}

bool BayesNet::addParent(ChanceNode& child, ChanceNode& parent) {
    if (child.net_ != this || parent.net_ != this || &child == &parent ||
        child.parents_.size() >= 16 ||
        std::ranges::find(child.parents_, &parent) != child.parents_.end()) {
        return false;
    }
    std::vector<ChanceNode*> pending{&child};
    std::unordered_set<ChanceNode*> visited;
    while (!pending.empty()) {
        auto* node = pending.back();
        pending.pop_back();
        if (node == &parent) {
            return false;
        }
        if (!visited.insert(node).second) {
            continue;
        }
        std::ranges::copy(node->children_, std::back_inserter(pending));
    }
    auto parents = child.parents_;
    parents.push_back(&parent);
    const auto count = tableSize(child.states_.size(), parents);
    if (count > MaxTable || totalTable(nodes_) - child.cpt_.size() + count > MaxTotalTable) {
        return false;
    }
    std::vector<Flt32> table(count, 1);
    parent.children_.reserve(parent.children_.size() + 1);
    child.parents_ = std::move(parents);
    child.cpt_ = std::move(table);
    parent.children_.push_back(&child);
    ++revision_;
    return true;
}

bool BayesNet::delParent(ChanceNode& child, ChanceNode& parent) {
    if (child.net_ != this || parent.net_ != this ||
        std::ranges::find(child.parents_, &parent) == child.parents_.end()) {
        return false;
    }
    auto parents = child.parents_;
    std::erase(parents, &parent);
    const auto count = tableSize(child.states_.size(), parents);
    if (count > MaxTable || totalTable(nodes_) - child.cpt_.size() + count > MaxTotalTable) {
        return false;
    }
    std::vector<Flt32> table(count, 1);
    child.parents_ = std::move(parents);
    child.cpt_ = std::move(table);
    std::erase(parent.children_, &child);
    ++revision_;
    return true;
}

bool BayesNet::delNode(ChanceNode& node) {
    if (node.net_ != this) {
        return false;
    }

    struct Replacement {
        ChanceNode* child;
        std::vector<ChanceNode*> parents;
        std::vector<Flt32> table;
    };

    std::vector<Replacement> replacements;
    auto total = totalTable(nodes_) - node.cpt_.size();
    for (auto* child : node.children_) {
        auto parents = child->parents_;
        std::erase(parents, &node);
        const auto count = tableSize(child->states_.size(), parents);
        total = total - child->cpt_.size() + count;
        if (count > MaxTable || total > MaxTotalTable) {
            return false;
        }
        replacements.push_back({child, std::move(parents), std::vector<Flt32>(count, 1)});
    }
    std::ranges::for_each(replacements, [](auto& value) {
        value.child->parents_ = std::move(value.parents);
        value.child->cpt_ = std::move(value.table);
    });
    std::ranges::for_each(node.parents_, [&](auto* parent) {
        std::erase(parent->children_, &node);
    });
    node.parents_.clear();
    node.children_.clear();
    std::erase(nodes_, &node);
    node.net_ = nullptr;
    std::erase_if(owned_, [&](const auto& pointer) {
        return pointer.get() == &node;
    });
    ++revision_;
    return true;
}

std::vector<ChanceNode*> BayesNet::topologicalOrder() const {
    // Legacy DFS visits children in insertion order, then pushes each node to
    // the front. Explicit frames avoid recursion even for a 4,096-node chain.
    struct Frame {
        ChanceNode* node;
        std::size_t child{};
    };

    std::vector<ChanceNode*> result;
    std::vector<Frame> stack;
    std::unordered_set<ChanceNode*> visited;
    for (auto* root : nodes_) {
        if (!visited.insert(root).second) {
            continue;
        }
        stack.push_back({root, 0});
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.child == frame.node->children_.size()) {
                result.push_back(frame.node);
                stack.pop_back();
                continue;
            }
            auto* child = frame.node->children_[frame.child++];
            if (visited.insert(child).second) {
                stack.push_back({child, 0});
            }
        }
    }
    std::ranges::reverse(result);
    return result;
}

bool BayesNet::evaluate(std::span<Flt32> values, std::size_t budget) const {
    if (values.size() != values_.size() || !std::ranges::all_of(values, [](auto f) {
            return std::isfinite(f);
        })) {
        return false;
    }
    // Evaluate transactionally: failures never publish partially computed states.
    std::vector<Flt32> result(values.begin(), values.end());
    for (const auto* node : topologicalOrder()) {
        if (node->parents_.empty()) {
            continue;
        }
        const auto combinations = tableSize(1, node->parents_);
        if (node->cpt_.size() != combinations * node->states_.size()) {
            return false;
        }
        for (auto state :
             std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t state) {
                 return state < node->states_.size();
             })) {
            std::vector<Flt32> row(
                node->cpt_.begin() + static_cast<std::ptrdiff_t>(state * combinations),
                node->cpt_.begin() + static_cast<std::ptrdiff_t>((state + 1) * combinations));
            for (const auto* parent : node->parents_ | std::views::reverse) {
                const auto count = parent->states_.size();
                if (!count) {
                    row.assign(1, 0);
                    break;
                }
                const auto rows = row.size() / count;
                if (row.size() > budget) {
                    return false;
                }
                budget -= row.size();
                for (auto i :
                     std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                         return i < rows;
                     })) {
                    Flt32 total = 0;
                    for (auto s : std::views::iota(std::size_t{0}) |
                                      std::views::take_while([&](std::size_t s) {
                                          return s < count;
                                      })) {
                        total += row[i * count + s] * result[parent->states_[s].valueIndex];
                    }
                    if (!std::isfinite(total)) {
                        return false;
                    }
                    row[i] = total;
                }
                row.resize(rows);
            }
            if (!(node->states_[state].flags & ChanceNode::ReadOnly)) {
                result[node->states_[state].valueIndex] = row.empty() ? 0 : row.front();
            }
        }
    }
    std::ranges::copy(result, values.begin());
    return true;
}

bool BayesNet::evaluate() {
    return evaluate(values_);
}

Persistent::Error
ChanceNode::write(StreamIO& stream, Int32, Int32, Persistent::StoreContext&) const {
    if (!stream.write(id_)) {
        return Persistent::Error::WriteError;
    }
    for (auto coordinate : rectangle) {
        if (!stream.write(coordinate)) {
            return Persistent::Error::WriteError;
        }
    }
    if (!writeName(stream, name_) || !stream.write(static_cast<Int32>(parents_.size()))) {
        return Persistent::Error::WriteError;
    }
    for (const auto* parent : parents_) {
        if (!stream.write(parent->id_)) {
            return Persistent::Error::WriteError;
        }
    }
    if (!stream.write(static_cast<Int32>(states_.size()))) {
        return Persistent::Error::WriteError;
    }
    for (const auto& state : states_) {
        if (!writeName(stream, state.name) || !stream.write(state.valueIndex) ||
            !stream.write(state.flags)) {
            return Persistent::Error::WriteError;
        }
    }
    if (!stream.write(static_cast<Int32>(cpt_.size()))) {
        return Persistent::Error::WriteError;
    }
    for (const auto probability : cpt_) {
        if (!stream.write(probability)) {
            return Persistent::Error::WriteError;
        }
    }
    return Persistent::Error::Ok;
}

Persistent::Error ChanceNode::read(StreamIO& stream, Int32, Int32, Persistent::LoadContext&) {
    if (net_ || !stream.read(id_) || id_ <= 0) {
        return Persistent::Error::ReadError;
    }
    for (auto& coordinate : rectangle) {
        if (!stream.read(coordinate)) {
            return Persistent::Error::ReadError;
        }
    }
    Int32 count{};
    if (!readName(stream, name_) || !stream.read(count) || count < 0 || count > 16) {
        return Persistent::Error::ReadError;
    }
    pendingParents_.resize(static_cast<std::size_t>(count));
    for (auto& parent : pendingParents_) {
        if (!stream.read(parent)) {
            return Persistent::Error::ReadError;
        }
    }
    if (!stream.read(count) || count < 0 || count > Int32{MaxValues}) {
        return Persistent::Error::ReadError;
    }
    states_.resize(static_cast<std::size_t>(count));
    for (auto& state : states_) {
        if (!readName(stream, state.name) || !stream.read(state.valueIndex) ||
            !stream.read(state.flags)) {
            return Persistent::Error::ReadError;
        }
    }
    if (!stream.read(count) || count < 0 || count > Int32{MaxTable}) {
        return Persistent::Error::ReadError;
    }
    cpt_.resize(static_cast<std::size_t>(count));
    for (auto& value : cpt_) {
        if (!stream.read(value) || !std::isfinite(value)) {
            return Persistent::Error::ReadError;
        }
    }
    return Persistent::Error::Ok;
}

Persistent::Error
BayesNet::write(StreamIO& stream, Int32, Int32 user, Persistent::StoreContext& context) const {
    if (!stream.write(nextId_) || !stream.write(static_cast<Int32>(values_.size()))) {
        return Persistent::Error::WriteError;
    }
    for (auto value : values_) {
        if (!stream.write(value)) {
            return Persistent::Error::WriteError;
        }
    }
    if (!stream.write(static_cast<Int32>(nodes_.size()))) {
        return Persistent::Error::WriteError;
    }
    for (const auto* node : topologicalOrder()) {
        const auto error = context.store(stream, *node, user);
        if (error != Persistent::Error::Ok) {
            return error;
        }
    }
    return Persistent::Error::Ok;
}

Persistent::Error BayesNet::read(StreamIO& stream, Int32, Int32, Persistent::LoadContext& context) {
    Int32 count{};
    if (!nodes_.empty() || !stream.read(nextId_) || nextId_ < 1 || !stream.read(count) ||
        count < 0 || count > Int32{MaxValues}) {
        return Persistent::Error::ReadError;
    }
    values_.resize(static_cast<std::size_t>(count));
    for (auto& value : values_) {
        if (!stream.read(value) || !std::isfinite(value)) {
            return Persistent::Error::ReadError;
        }
    }
    if (!stream.read(count) || count < 0 || count > Int32{MaxNodes}) {
        return Persistent::Error::ReadError;
    }
    std::unordered_set<UInt32> indices;
    for ([[maybe_unused]] auto i :
         std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
             return i < count;
         })) {
        const auto result = context.load(stream);
        if (!result) {
            return result.error;
        }
        auto* node = dynamic_cast<ChanceNode*>(result.object);
        if (!node || node->net_ || node->id_ >= nextId_ || findNode(node->id_)) {
            return Persistent::Error::ReadError;
        }
        for (const auto& state : node->states_) {
            if (state.valueIndex >= values_.size() || !indices.insert(state.valueIndex).second) {
                return Persistent::Error::ReadError;
            }
        }
        if (totalTable(nodes_) + node->cpt_.size() > MaxTotalTable) {
            return Persistent::Error::LimitExceeded;
        }
        node->net_ = this;
        nodes_.push_back(node);
    }
    for (auto* node : nodes_) {
        auto probabilities = node->cpt_;
        for (auto parentId : node->pendingParents_) {
            auto* parent = findNode(parentId);
            if (!parent || !addParent(*node, *parent)) {
                return Persistent::Error::ReadError;
            }
        }
        if (tableSize(node->states_.size(), node->parents_) != probabilities.size()) {
            return Persistent::Error::ReadError;
        }
        node->cpt_ = std::move(probabilities);
        node->pendingParents_.clear();
    }
    ++revision_;
    return Persistent::Error::Ok;
}

BayesNetInstance::BayesNetInstance(std::shared_ptr<const BayesNet> net)
    : net_(net.get()), shared_(std::move(net)) {
    if (net_) {
        values_.assign(net_->stateValues().begin(), net_->stateValues().end());
        revision_ = net_->revision();
    }
}

BayesNetInstance::BayesNetInstance(Resource<BayesNet> net)
    : net_(net.get()), resource_(std::move(net)) {
    if (net_) {
        values_.assign(net_->stateValues().begin(), net_->stateValues().end());
        revision_ = net_->revision();
    }
}

bool BayesNetInstance::registerStateVariable(std::string_view name,
                                             std::function<Flt32()> get,
                                             std::function<void(Flt32)> set) {
    if (!net_ || net_->revision() != revision_) {
        return false;
    }
    for (const auto* node : net_->nodes()) {
        for (const auto& state : node->states()) {
            if (equalName(name, state.name)) {
                std::erase_if(bindings_, [&](const auto& binding) {
                    return binding.index == state.valueIndex;
                });
                if (get && set) {
                    bindings_.push_back({state.valueIndex, std::move(get), std::move(set)});
                }
                return true;
            }
        }
    }
    return false;
}

bool BayesNetInstance::evaluateNetwork() {
    if (!net_ || net_->revision() != revision_) {
        return false;
    }
    auto values = values_;
    std::ranges::for_each(bindings_, [&](const auto& binding) {
        values[binding.index] = binding.get();
    });
    if (!net_->evaluate(values)) {
        return false;
    }
    values_ = std::move(values);
    std::ranges::for_each(bindings_, [&](const auto& binding) {
        binding.set(values_[binding.index]);
    });
    return true;
}

bool registerBayesClasses(Persistent::ClassRegistry& registry) {
    return registry.registerClass<ChanceNode>("ChanceNode", makeFourCC('B', 'N', 'O', 'D')) &&
           registry.registerClass<BayesNet>("BayesNet", makeFourCC('B', 'N', 'E', 'T'));
}

bool registerBayesResource(ResourceManager& resources) {
    // The returned root retains the registry/context that owns its loaded nodes.
    struct Loaded {
        Persistent::ClassRegistry registry;
        Persistent::LoadContext context{registry};
        BayesNet* net{};

        Loaded() {
            registerBayesClasses(registry);
        }
    };

    return resources.registerLoader<BayesNet>(
        ".ai", [](StreamIO& stream, UInt32) -> std::unique_ptr<BayesNet> {
            auto loaded = std::make_shared<Loaded>();
            const auto result = loaded->context.load(stream);
            loaded->net = result ? dynamic_cast<BayesNet*>(result.object) : nullptr;
            if (!loaded->net) {
                return {};
            }
            auto root = std::make_unique<BayesNet>();
            root->nodes_ = std::move(loaded->net->nodes_);
            root->values_ = std::move(loaded->net->values_);
            root->nextId_ = loaded->net->nextId_;
            root->revision_ = loaded->net->revision_;
            std::ranges::for_each(root->nodes_, [&](auto* node) {
                node->net_ = root.get();
            });
            root->loadedOwner_ = std::move(loaded);
            return root;
        });
}
} // namespace opent1::sim
