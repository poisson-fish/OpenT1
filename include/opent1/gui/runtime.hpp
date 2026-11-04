#pragma once
#include <opent1/gui/controls.hpp>
#include <opent1/gui/edit_control.hpp>
#include <opent1/console/console.hpp>

namespace opent1::gui {
// A graph owns every decoded control. Canvas only observes it. Resource binding
// stages every lookup before changing any control, so missing assets are retryable.
struct Resources {
    std::function<std::shared_ptr<const gfx::BitmapFont>(Int32)> font;
    std::function<std::shared_ptr<const gfx::Bitmap>(Int32)> bitmap;
    std::function<std::shared_ptr<const gfx::BitmapArray>(Int32)> array;
    std::function<std::shared_ptr<const gfx::Palette>(Int32)> palette;
    std::function<std::optional<std::string>(Int32)> string;
    std::function<bool(Control&)> custom;
};

// Typed Resource handles are retained through aliasing shared owners; tags and
// resource manager may expire after a graph has been bound successfully.
[[nodiscard]] Resources resourceBindings(std::shared_ptr<ResourceManager>,
                                         std::shared_ptr<const TagDictionary>);
bool bindResources(Control&, const Resources&);

class Graph {
public:
    static std::shared_ptr<Graph>
    load(StreamIO&, const Persistent::ClassRegistry&, const Resources&);
    static std::shared_ptr<Graph> create(std::unique_ptr<Control>);

    Control& root() const {
        return *root_;
    }

private:
    friend class Runtime;
    std::vector<std::unique_ptr<Persistent::Base>> objects_;
    Control* root_{};
};

class Delegate {
public:
    virtual ~Delegate() = default;

    virtual void gained() noexcept {}

    virtual void lost() noexcept {}

    virtual void regained() noexcept {}

    virtual void contentChanged(Control*, Control*) noexcept {}

    virtual void opened(Control&) noexcept {}

    virtual void dialogPushed(Control*, Control*) noexcept {}

    virtual void dialogPopped(Control*) noexcept {}

    virtual void dialogPushed(Control* old, Control* next, UInt32) noexcept {
        dialogPushed(old, next);
    }

    // Preparation may veto/throw without publishing a graph or delegation.
    virtual bool canPop(Control*, UInt32) {
        return true;
    }

    virtual bool message(Control&, UInt32) {
        return true;
    }

    // Empty forwards to the next older delegate. False refuses the transaction;
    // true consumes it. The legacy bool hook keeps its acceptance contract.
    virtual std::optional<bool> routeMessage(Control& c, UInt32 m) {
        return message(c, m) ? std::nullopt : std::optional(false);
    }

    virtual bool event(const Event&) {
        return false;
    }

    virtual void help(const Canvas::HelpState&) {}
};

// Console outlives the running Runtime (stop before host teardown). The reference
// registry overload borrows; the shared overload retains it. Graphs/resources
// and delegation are owned; command callbacks retain only a weak runtime.
// Deferred graph changes require shared Runtime ownership.
class Runtime : public std::enable_shared_from_this<Runtime> {
public:
    Runtime(Console&, const Persistent::ClassRegistry&, Resources = {});
    ~Runtime();
    Runtime(Console&, std::shared_ptr<const Persistent::ClassRegistry>, Resources = {});
    Canvas canvas;
    bool load(StreamIO&, bool dialog = false);
    bool show(std::shared_ptr<Graph>,
              bool dialog = false,
              std::shared_ptr<Delegate> nextDelegate = {},
              UInt32 message = 0);
    bool open(std::string_view, bool dialog = false, UInt32 message = 0);
    bool pop(UInt32 message = 0);
    bool clear();
    void delegate(std::shared_ptr<Delegate>);

    [[nodiscard]] std::shared_ptr<Delegate> currentDelegate() const {
        return delegate_;
    }

    bool synchronizeVariables();
    [[nodiscard]] Control* findControl(std::string_view) const;
    void install();
    void uninstall();
    void stop();
    std::function<std::optional<std::vector<std::byte>>(std::string_view)> loadFile;
    std::function<void(std::string_view)> loadFailed;
    std::function<std::shared_ptr<Delegate>(std::string_view)> loadDelegate;
    bool save(const std::filesystem::path&) const;
    bool editing(bool);

    [[nodiscard]] EditControl* editor() const {
        return editor_.get();
    }

    [[nodiscard]] bool running() const {
        return !stopped_;
    }

    [[nodiscard]] const Console& console() const {
        return console_;
    }

    // Both the native canvas-qualified spelling and the single-runtime spelling
    // are accepted. Controls resolve from the top dialog through the content.
    std::optional<std::string> command(std::string_view, std::span<const std::string>);
    std::function<bool(std::string_view, std::span<const std::byte>)> saveFile;
    std::function<std::optional<Canvas::Cursor>(std::string_view)> loadCursor;
    std::function<bool(bool mouse, bool enabled)> inputEnabled;
    std::function<bool(EditControl&)> toolbar;
    std::function<bool(Control*)> inspect;

    [[nodiscard]] std::size_t graphCount() const {
        return graphs_.size();
    }

private:
    bool action(Control&, UInt32, std::string_view);
    bool routeMessage(Control&, UInt32);
    bool routeEvent(const Event&);
    Console& console_;
    std::shared_ptr<const Persistent::ClassRegistry> registryOwner_;
    const Persistent::ClassRegistry& registry_;
    Resources resources_;
    std::vector<std::shared_ptr<Graph>> graphs_;
    std::shared_ptr<Delegate> delegate_;
    // One effective responder per graph; inherited entries share ownership.
    std::vector<std::shared_ptr<Delegate>> delegates_;
    std::unique_ptr<EditControl> editor_;
    std::vector<std::byte> selectionClipboard_;
    bool installed_{}, stopped_{}, changing_{};
    Control* scriptRoot_{};
    std::vector<std::shared_ptr<void>> commands_;
};
} // namespace opent1::gui
