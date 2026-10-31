#pragma once
#include <opent1/console/console.hpp>
#include <opent1/core/persist.hpp>
#include <opent1/core/types.hpp>
#include <opent1/sim/sim_base.hpp>
#include <opent1/sim/sim_input_action.hpp>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

class ChatMenu : public sim::SimObject {
    using Parent = sim::SimObject;

public:
    enum Mode : Int32 {
        Inactive = 0,
        Active = 1,
        OneShot = 2,
    };

    enum Constants : Int32 {
        MaxHeadingLen = 31,
        MaxArgs = 16,
    };

    static inline constexpr UInt8 kEscapeScanCode = 0x01;
    static inline constexpr Int32 kChatMenuInputPriority = 2001;

    struct Node {
        char key{'\0'};
        std::string heading{};
        std::unique_ptr<Node> child{};
        std::unique_ptr<Node> nextSibling{};
        Int32 argc{0};
        std::array<std::string, MaxArgs> argv{};
    };

    ChatMenu();
    explicit ChatMenu(std::string_view initialHeading);
    ~ChatMenu() override;

    void bindConsole(Console* console) noexcept {
        boundConsole_ = console;
    }

    [[nodiscard]] Console* resolveConsole() const noexcept {
        return boundConsole_;
    }

    void clearMenu();
    void setHeading(std::string_view newHeading);

    [[nodiscard]] std::string_view getHeading() const noexcept {
        return heading_;
    }

    bool addMenuString(std::string_view menuString, std::span<const std::string_view> argv);
    bool addMenuString(std::string_view menuString, std::span<const std::string> argv);
    bool processKey(char key);
    void setMode(Int32 mode);

    [[nodiscard]] Int32 getMode() const noexcept {
        return curMode_;
    }

    [[nodiscard]] const Node* rootMenu() const noexcept {
        return chatTree_.get();
    }

    [[nodiscard]] const Node* curMenu() const noexcept {
        return curMenu_;
    }

    [[nodiscard]] std::string_view curMenuQuery() const noexcept {
        return curMenuQuery_;
    }

    [[nodiscard]] UInt32 lastKey() const noexcept {
        return lastKey_;
    }

    [[nodiscard]] std::span<const std::vector<std::string>> executedCommands() const noexcept {
        return executedCommands_;
    }

    void clearExecutedCommands() noexcept {
        executedCommands_.clear();
    }

    bool processArguments(std::span<const std::string> arguments) override;
    bool onAdd() override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

private:
    std::string heading_{"Options:"};
    std::unique_ptr<Node> chatTree_{};
    Node* curMenu_{nullptr};
    Int32 curMode_{Inactive};
    UInt32 lastKey_{0};
    std::string curMenuQuery_{};
    Console* boundConsole_{nullptr};
    std::vector<std::vector<std::string>> executedCommands_{};
};

bool registerChatMenuClass(Persistent::ClassRegistry& registry);

} // namespace opent1::game
