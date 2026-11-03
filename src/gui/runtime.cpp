#include <opent1/gui/runtime.hpp>
#include <ranges>
#include <opent1/gui/containers.hpp>
#include <opent1/gui/text_format.hpp>
#include <opent1/core/mem_stream.hpp>
#include <charconv>
#include <cmath>
#include <unordered_set>

namespace opent1::gui {
namespace {
const Persistent::ClassRegistry&
requireRegistry(const std::shared_ptr<const Persistent::ClassRegistry>& registry) {
    if (!registry) {
        throw std::invalid_argument("GUI registry");
    }
    return *registry;
}

template <class T>
std::shared_ptr<const T> tagged(const std::shared_ptr<ResourceManager>& manager,
                                const std::shared_ptr<const TagDictionary>& tags,
                                Int32 tag) {
    if (!manager || !tags) {
        return {};
    }
    const auto name = tags->idToString(tag);
    if (!name) {
        return {};
    }
    auto resource = manager->load<T>(*name);
    if (!resource) {
        return {};
    }
    auto owner = std::make_shared<Resource<T>>(std::move(resource));
    return std::shared_ptr<const T>(owner, owner->get());
}

constexpr std::array names{"GuiLoadContentCtrl",
                           "GuiPushDialog",
                           "GuiPopDialog",
                           "GuiSaveContentCtrl",
                           "Control::setText",
                           "Control::getText",
                           "Control::setVisible",
                           "Control::getVisible",
                           "Control::setActive",
                           "Control::getActive",
                           "Control::performClick",
                           "TextList::Clear",
                           "TextList::AddLine",
                           "GuiEditMode",
                           "GuiEditNewControl",
                           "GuiSetAddSet",
                           "GuiSetSelection",
                           "GuiNewContentCtrl",
                           "GuiSaveSelection",
                           "GuiLoadSelection",
                           "GuiInspect",
                           "GuiJustify",
                           "GuiToolbar",
                           "GuiSendToBack",
                           "GuiBringToFront",
                           "GuiSendRootMessage",
                           "setCursor",
                           "isCursorOn",
                           "cursorOn",
                           "cursorOff",
                           "windowsMouseEnable",
                           "windowsMouseDisable",
                           "windowsKeyboardEnable",
                           "windowsKeyboardDisable",
                           "Control::setValue",
                           "Control::getValue",
                           "GuiUndo",
                           "GuiRedo",
                           "GuiDeleteSelection"};

std::string scriptValue(Control& c) {
    auto custom = c.getScriptValue();
    if (!custom.empty()) {
        return custom;
    }
    if (auto* check = dynamic_cast<Check*>(&c)) {
        return check->checked ? "True" : "False";
    }
    if (auto* slider = dynamic_cast<Slider*>(&c)) {
        return std::to_string(slider->value);
    }
    if (auto* text = dynamic_cast<Text*>(&c)) {
        return text->text;
    }
    if (auto* format = dynamic_cast<TextFormat*>(&c)) {
        return format->text();
    }
    if (auto* list = dynamic_cast<TextList*>(&c)) {
        auto* entry = list->selection();
        return entry ? entry->text : std::string{};
    }
    return {};
}

bool setValue(Control& c, std::string_view value) {
    if (auto* format = dynamic_cast<TextFormat*>(&c)) {
        return format->format(value, std::max(20, c.fields().extent.x)) &&
               format->resize(c.fields().position, {c.fields().extent.x, format->height() + 4});
    }
    if (auto* list = dynamic_cast<TextList*>(&c)) {
        return list->variableChanged(value);
    }
    if (auto* wrap = dynamic_cast<TextWrap*>(&c)) {
        return wrap->variableChanged(value);
    }
    if (auto* progress = dynamic_cast<Progress*>(&c)) {
        float f{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), f);
        return result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
               progress->percent(f);
    }
    if (auto* slider = dynamic_cast<Slider*>(&c)) {
        if (slider->variableChanged(value)) {
            return true;
        }
        float f{};
        auto r = std::from_chars(value.data(), value.data() + value.size(), f);
        return r.ec == std::errc{} && r.ptr == value.data() + value.size() && slider->setValue(f);
    }
    if (auto* text = dynamic_cast<Text*>(&c)) {
        return text->variableChanged(value);
    }
    return c.variableChanged(value);
}
}

Resources resourceBindings(std::shared_ptr<ResourceManager> manager,
                           std::shared_ptr<const TagDictionary> tags) {
    Resources result;
    result.font = [manager, tags](Int32 tag) {
        return tagged<gfx::BitmapFont>(manager, tags, tag);
    };
    result.bitmap = [manager, tags](Int32 tag) {
        return tagged<gfx::Bitmap>(manager, tags, tag);
    };
    result.array = [manager, tags](Int32 tag) {
        return tagged<gfx::BitmapArray>(manager, tags, tag);
    };
    result.palette = [manager, tags](Int32 tag) {
        return tagged<gfx::Palette>(manager, tags, tag);
    };
    result.string = [tags](Int32 tag) -> std::optional<std::string> {
        if (!tags) {
            return {};
        }
        auto value = tags->idToString(tag);
        return value ? std::optional(std::string(*value)) : std::nullopt;
    };
    return result;
}

bool bindResources(Control& root, const Resources& resources) {
    std::vector<std::function<void()>> commit;
    const auto bind = [&](auto& target, Int32 tag, const auto& loader) -> bool {
        if (!tag) {
            return true;
        }
        if (!loader) {
            return false;
        }
        auto next = loader(tag);
        if (!next || !next->valid()) {
            return false;
        }
        commit.push_back([&target, next = std::move(next)] {
            target = next;
        });
        return true;
    };
    const auto visit = [&](auto&& self, Control& c) -> bool {
        if (auto* t = dynamic_cast<Text*>(&c)) {
            if (!bind(t->font, Int32(t->fontTag), resources.font) ||
                !bind(t->highlightFont, Int32(t->highlightFontTag), resources.font) ||
                !bind(t->disabledFont, Int32(t->disabledFontTag), resources.font)) {
                return false;
            }
            if (t->textTag) {
                if (!resources.string) {
                    return false;
                }
                auto value = resources.string(Int32(t->textTag));
                if (!value || value->size() > 65536 || value->find('\0') != std::string::npos) {
                    return false;
                }
                if (auto* edit = dynamic_cast<TextEdit*>(t);
                    edit && value->size() > edit->maxBytes) {
                    return false;
                }
                commit.push_back([t, value = std::move(*value)]() mutable {
                    t->text.swap(value);
                    if (auto* edit = dynamic_cast<TextEdit*>(t)) {
                        edit->cursor = edit->anchor = t->text.size();
                    }
                });
            }
        }
        if (auto* b = dynamic_cast<Bitmap*>(&c);
            b && !bind(b->bitmap, b->bitmapTag, resources.bitmap)) {
            return false;
        }
        if (auto* a = dynamic_cast<Animate*>(&c); a && a->arrayTag) {
            auto next = resources.array ? resources.array(a->arrayTag) : nullptr;
            if (!next || !next->valid() || next->bitmaps.empty()) {
                return false;
            }
            for (auto i : std::views::iota(std::size_t{0}, next->bitmaps.size())) {
                if (!next->get(Int32(i))) {
                    return false;
                }
            }
            const auto* first = next->get(0);
            const Point2I extent{first->width, first->height};
            commit.push_back([a, next = std::move(next), extent] {
                if (!a->resize(a->fields().position, extent)) {
                    throw std::runtime_error("GUI animation extent");
                }
                a->frames = next;
            });
        }
        if (auto* b = dynamic_cast<BitmapBox*>(&c)) {
            const std::function<std::shared_ptr<const gfx::BitmapArray>(Int32)> boxArray =
                [&](Int32 tag) -> std::shared_ptr<const gfx::BitmapArray> {
                auto next = resources.array ? resources.array(tag) : nullptr;
                if (!next) {
                    return {};
                }
                for (auto i : std::views::iota(Int32{0}, Int32{9})) {
                    if (!next->get(i)) {
                        return {};
                    }
                }
                return next;
            };
            if (!bind(b->frames, b->arrayTag, boxArray)) {
                return false;
            }
        }
        if (auto* combo = dynamic_cast<ComboBox*>(&c); combo && resources.array) {
            // The original onAdd also creates/adopts its transient ScrollCtrl.
            // Stage its default art with the persisted graph's other bindings.
            const std::function<std::shared_ptr<const gfx::BitmapArray>(Int32)> popupArt =
                [&](Int32 tag) {
                    auto next = resources.array(tag);
                    return next && next->bitmaps.size() >= 25 ? next : nullptr;
                };
            if (!bind(combo->popupScroll().frames, 179998, popupArt)) {
                return false;
            }
        }
        if (auto* b = dynamic_cast<Scroll*>(&c)) {
            // Native indexes the scroll parts during onAdd; a valid but
            // shorter array would dereference beyond the resource table.
            const std::function<std::shared_ptr<const gfx::BitmapArray>(Int32)> scrollArray =
                [&](Int32 tag) {
                    auto next = resources.array ? resources.array(tag) : nullptr;
                    return next && next->bitmaps.size() >= 25 ? next : nullptr;
                };
            if (!bind(b->frames, b->arrayTag, scrollArray)) {
                return false;
            }
        }
        if (auto* f = dynamic_cast<TextFormat*>(&c)) {
            for (auto& font : f->fonts) {
                if (!bind(font.resource, font.tag, resources.font)) {
                    return false;
                }
            }
        }
        if (auto* p = dynamic_cast<PaletteControl*>(&c);
            p && !bind(p->palette, p->paletteTag, resources.palette)) {
            return false;
        }
        if (auto* l = dynamic_cast<TextList*>(&c)) {
            // Native lists retry the default font for zero and missing tags.
            // A programmatic list with an already supplied font needs no loader.
            const auto listFont = [&](auto& target, Int32 tag) {
                if (!tag && target) {
                    return true;
                }
                if (!resources.font) {
                    return !tag;
                }
                auto next = tag ? resources.font(tag) : nullptr;
                if (!next || !next->valid()) {
                    next = resources.font(159998);
                }
                if (!next || !next->valid()) {
                    return false;
                }
                commit.push_back([&target, next = std::move(next)] {
                    target = next;
                });
                return true;
            };
            if (!listFont(l->font, l->fontTag) ||
                !listFont(l->highlightFont, l->highlightFontTag)) {
                return false;
            }
        }
        if (auto* t = dynamic_cast<TextWrap*>(&c)) {
            if (!bind(t->font, t->fontTag, resources.font) ||
                !bind(t->disabledFont, t->disabledFontTag, resources.font)) {
                return false;
            }
            if (t->textTag) {
                if (!resources.string) {
                    return false;
                }
                auto value = resources.string(t->textTag);
                if (!value) {
                    return false;
                }
                TextWrap staged;
                if (!staged.setText(*value)) {
                    return false;
                }
                commit.push_back([t, value = std::move(staged.text)]() mutable {
                    t->text.swap(value);
                });
            }
        }
        for (auto* o : c.objects()) {
            if (!self(self, *static_cast<Control*>(o))) {
                return false;
            }
        }
        return true;
    };
    std::vector<std::function<void()>> undo;
    const auto save = [&](auto&& self, Control& c) -> void {
        undo.push_back(c.checkpoint());
        for (auto* child : c.objects()) {
            self(self, *static_cast<Control*>(child));
        }
    };
    try {
        if (!visit(visit, root)) {
            return false;
        }
        save(save, root);
        const auto custom = [&](auto&& self, Control& c) -> bool {
            if (resources.custom && !resources.custom(c)) {
                return false;
            }
            for (auto* child : c.objects()) {
                if (!self(self, *static_cast<Control*>(child))) {
                    return false;
                }
            }
            return true;
        };
        for (auto& publish : commit) {
            publish();
        }
        if (!custom(custom, root)) {
            for (auto& restore : undo) {
                restore();
            }
            return false;
        }
    } catch (...) {
        for (auto& restore : undo) {
            restore();
        }
        return false;
    }
    return true;
}

std::shared_ptr<Graph> Graph::load(StreamIO& stream,
                                   const Persistent::ClassRegistry& registry,
                                   const Resources& resources) {
    Persistent::LoadContext load(registry);
    auto result = load.load(stream);
    if (!result) {
        return {};
    }
    auto* root = dynamic_cast<Control*>(result.object);
    if (!root || root->parent() || !bindResources(*root, resources)) {
        return {};
    }
    auto graph = std::make_shared<Graph>();
    graph->root_ = root;
    graph->objects_ = load.takeObjects();
    return graph;
}

std::shared_ptr<Graph> Graph::create(std::unique_ptr<Control> root) {
    if (!root || root->parent() || root->canvas() || root->getManager()) {
        return {};
    }
    auto graph = std::make_shared<Graph>();
    graph->root_ = root.get();
    graph->objects_.push_back(std::move(root));
    return graph;
}

Runtime::Runtime(Console& console, const Persistent::ClassRegistry& registry, Resources resources)
    : console_(console), registry_(registry), resources_(std::move(resources)) {
    canvas.onAction = [this](auto& c, auto message, auto command) {
        return action(c, message, command);
    };
    canvas.onMessage = [this](auto& c, auto message) {
        return routeMessage(c, message);
    };
    canvas.onUnhandled = [this](const auto& e) {
        return routeEvent(e);
    };
    canvas.onHelp = [this](const auto& state) {
        auto owner = delegate_;
        if (owner) {
            owner->help(state);
        }
    };
    canvas.controlCursor = [this](Int32 tag) -> std::optional<Canvas::Cursor> {
        if (!resources_.string || !loadCursor) {
            return {};
        }
        const auto definition = resources_.string(tag);
        if (!definition || definition->size() > 255 ||
            definition->find('\0') != std::string::npos) {
            return {};
        }
        const auto comma = definition->find(',');
        auto result = loadCursor(std::string_view(*definition).substr(0, comma));
        if (!result) {
            return {};
        }
        if (comma != std::string::npos) {
            auto values = std::string_view(*definition).substr(comma + 1);
            std::array<double, 3> parsed{};
            std::size_t count = 0;
            while (!values.empty() && count < 3) {
                const auto end = values.find(',');
                auto part = values.substr(0, end);
                const auto begin = part.find_first_not_of(" \t\r\n");
                if (begin == part.npos) {
                    return {};
                }
                part = part.substr(begin, part.find_last_not_of(" \t\r\n") - begin + 1);
                auto [last, error] =
                    std::from_chars(part.data(), part.data() + part.size(), parsed[count]);
                if (error != std::errc{} || last != part.data() + part.size() ||
                    !std::isfinite(parsed[count])) {
                    return {};
                }
                ++count;
                if (end == std::string::npos) {
                    values = {};
                } else {
                    values.remove_prefix(end + 1);
                }
            }
            if (!values.empty() || count < 2 || std::abs(parsed[0]) > Control::MaxCoordinate ||
                std::abs(parsed[1]) > Control::MaxCoordinate ||
                std::floor(parsed[0]) != parsed[0] || std::floor(parsed[1]) != parsed[1]) {
                return {};
            }
            result->hotspot = {Int32(parsed[0]), Int32(parsed[1])};
            if (count == 3) {
                if (parsed[2] <= 0) {
                    return {};
                }
                result->frameTime = parsed[2];
            }
        }
        return result;
    };
}

Runtime::Runtime(Console& console,
                 std::shared_ptr<const Persistent::ClassRegistry> registry,
                 Resources resources)
    : Runtime(console, requireRegistry(registry), std::move(resources)) {
    registryOwner_ = std::move(registry);
}

Runtime::~Runtime() {
    uninstall();
    try {
        editing(false);
    } catch (...) {
        canvas.cancel();
        editor_.reset();
    }
    canvas.content(nullptr);
    graphs_.clear();
    if (delegate_) {
        delegate_->lost();
    }
}

bool Runtime::load(StreamIO& s, bool dialog) {
    auto graph = Graph::load(s, registry_, resources_);
    return graph && show(std::move(graph), dialog);
}

bool Runtime::show(std::shared_ptr<Graph> graph,
                   bool dialog,
                   std::shared_ptr<Delegate> nextDelegate,
                   UInt32 message) {
    if (!graph || stopped_ || changing_ || canvas.dispatching() || canvas.changingContent()) {
        return false;
    }
    changing_ = true;

    struct Reset {
        bool& flag;

        ~Reset() {
            flag = false;
        }
    } reset{changing_};

    auto* old = canvas.top();
    auto next = graphs_;
    auto nextDelegates = delegates_;
    auto previous = delegate_;
    if (dialog) {
        if (nextDelegate && std::ranges::find(delegates_, nextDelegate) != delegates_.end()) {
            return false;
        }
        next.push_back(graph);
        nextDelegates.push_back(nextDelegate ? nextDelegate : delegate_);
    } else {
        next = {graph};
        nextDelegates = {nextDelegate ? nextDelegate
                                      : (delegates_.empty() ? delegate_ : delegates_.front())};
    }
    try {
        if (!dialog) {
            for (std::size_t i = graphs_.size(); i > 1; --i) {
                if (delegates_[i - 1] && !delegates_[i - 1]->canPop(&graphs_[i - 1]->root(), 0)) {
                    return false;
                }
            }
        }
    } catch (...) {
        return false;
    }
    // Native adoption applies bound variables before wake. Prepare the candidate
    // without exposing it, and restore it when binding or Canvas wake refuses.
    std::vector<std::function<void()>> undo;
    const auto prepare = [&](auto&& self, Control& c) -> bool {
        if (!c.fields().variable.empty()) {
            undo.push_back(c.checkpoint());
            if (!setValue(c, console_.getVariable(c.fields().variable))) {
                return false;
            }
        }
        for (auto* child : c.objects()) {
            if (!self(self, *static_cast<Control*>(child))) {
                return false;
            }
        }
        return true;
    };
    try {
        if (!prepare(prepare, graph->root()) ||
            !(dialog ? canvas.pushDialog(graph->root()) : canvas.content(&graph->root()))) {
            for (auto& restore : undo) {
                restore();
            }
            return false;
        }
    } catch (...) {
        for (auto& restore : undo) {
            restore();
        }
        return false;
    }
    editing(false);
    // Publish ownership before notifying. Old graphs/delegates remain alive
    // through every callback; nested mutations are refused by changing_.
    auto oldGraphs = std::move(graphs_);
    auto oldDelegates = std::move(delegates_);
    graphs_ = std::move(next);
    delegates_ = std::move(nextDelegates);
    delegate_ = delegates_.back();
    if (dialog) {
        if (previous) {
            previous->dialogPushed(old, &graph->root(), message);
        }
        if (nextDelegate) {
            nextDelegate->gained();
        }
    } else {
        if (!oldGraphs.empty()) {
            // onClose still reads the outgoing screen's fields (player config).
            scriptRoot_ = &oldGraphs.front()->root();

            struct RestoreRoot {
                Control*& root;

                ~RestoreRoot() {
                    root = nullptr;
                }
            } restoreRoot{scriptRoot_};

            const auto hook = std::string(scriptRoot_->getName()) + "::onClose";
            if (console_.isFunction(hook)) {
                static_cast<void>(console_.execute(hook));
            }
        }
        for (std::size_t i = oldGraphs.size(); i > 1; --i) {
            if (oldDelegates[i - 1] != oldDelegates[i - 2]) {
                if (oldDelegates[i - 1]) {
                    oldDelegates[i - 1]->lost();
                }
                if (oldDelegates[i - 2]) {
                    oldDelegates[i - 2]->regained();
                }
            }
        }
        const auto base = oldDelegates.empty() ? previous : oldDelegates.front();
        if (base != delegate_) {
            if (base) {
                base->lost();
            }
            if (delegate_) {
                delegate_->gained();
            }
        }
        if (delegate_) {
            delegate_->contentChanged(oldGraphs.empty() ? nullptr : &oldGraphs.front()->root(),
                                      &graph->root());
        }
        canvas.tab();
        // Adoption is complete. The script may immediately open a first-player
        // dialog, and subsequent commands must see that dialog synchronously.
        changing_ = false;
        const auto hook = std::string(graph->root().getName()) + "::onOpen";
        if (console_.isFunction(hook)) {
            static_cast<void>(console_.execute(hook));
        }
        if (delegate_ && canvas.top() == &graph->root()) {
            delegate_->opened(graph->root());
        }
    }
    return true;
}

bool Runtime::pop(UInt32 message) {
    if (stopped_ || changing_ || canvas.dispatching() || canvas.changingContent() ||
        graphs_.size() < 2) {
        return false;
    }
    changing_ = true;

    struct Reset {
        bool& flag;

        ~Reset() {
            flag = false;
        }
    } reset{changing_};

    auto old = graphs_.back();
    auto previous = delegate_;
    try {
        if (previous && !previous->canPop(&old->root(), message)) {
            return false;
        }
    } catch (...) {
        return false;
    }
    if (!canvas.popDialog()) {
        return false;
    }
    editing(false);
    graphs_.pop_back();
    delegates_.pop_back();
    delegate_ = delegates_.back();
    if (previous) {
        previous->dialogPopped(&old->root());
    }
    if (previous != delegate_) {
        if (previous) {
            previous->lost();
        }
        if (delegate_) {
            delegate_->regained();
        }
    }
    return true;
}

bool Runtime::clear() {
    if (stopped_ || changing_ || canvas.dispatching() || canvas.changingContent()) {
        return false;
    }
    changing_ = true;

    struct Reset {
        bool& flag;

        ~Reset() {
            flag = false;
        }
    } reset{changing_};

    try {
        for (std::size_t i = graphs_.size(); i > 1; --i) {
            if (delegates_[i - 1] && !delegates_[i - 1]->canPop(&graphs_[i - 1]->root(), 0)) {
                return false;
            }
        }
    } catch (...) {
        return false;
    }
    if (!canvas.content(nullptr)) {
        return false;
    }
    editing(false);
    for (std::size_t i = delegates_.size(); i > 1; --i) {
        if (delegates_[i - 1] != delegates_[i - 2]) {
            if (delegates_[i - 1]) {
                delegates_[i - 1]->lost();
            }
            if (delegates_[i - 2]) {
                delegates_[i - 2]->regained();
            }
        }
    }
    if (!delegates_.empty()) {
        delegate_ = delegates_.front();
    }
    delegates_.clear();
    graphs_.clear();
    return true;
}

void Runtime::delegate(std::shared_ptr<Delegate> next) {
    if (changing_ || canvas.dispatching() || canvas.changingContent() || next == delegate_) {
        return;
    }
    changing_ = true;

    struct Reset {
        bool& flag;

        ~Reset() {
            flag = false;
        }
    } reset{changing_};

    auto previous = delegate_;
    delegate_ = std::move(next);
    if (!delegates_.empty()) {
        auto i = delegates_.size();
        do {
            --i;
            if (delegates_[i] != previous) {
                break;
            }
            delegates_[i] = delegate_;
        } while (i);
    }
    if (previous) {
        previous->lost();
    }
    if (delegate_) {
        delegate_->gained();
    }
}

bool Runtime::open(std::string_view path, bool dialog, UInt32 message) {
    if (stopped_ || changing_ || path.empty() || path.size() > 240 ||
        path.find('\0') != std::string_view::npos || !loadFile) {
        return false;
    }
    changing_ = true;

    struct Reset {
        bool& flag;

        ~Reset() {
            flag = false;
        }
    } reset{changing_};

    std::shared_ptr<Graph> graph;
    std::shared_ptr<Delegate> next;
    try {
        auto bytes = loadFile(path);
        if (!bytes || bytes->size() > 16U * 1024U * 1024U) {
            return false;
        }
        MemRWStream in{std::span<const std::byte>(*bytes)};
        graph = Graph::load(in, registry_, resources_);
        if (!graph || in.getPosition() != Int32(bytes->size())) {
            return false;
        }
        auto file = path.substr(path.find_last_of("/\\") + 1);
        auto dot = file.find('.');
        if (dot == std::string_view::npos) {
            dot = file.size();
        }
        if (!dot) {
            return false;
        }
        if (!dialog) {
            graph->root().assignName(std::string(file.substr(0, dot)) + "Gui");
        }
        if (loadDelegate) {
            next = loadDelegate(std::string("SimGui::") + std::string(file.substr(0, dot)) +
                                "Delegate");
        }
    } catch (...) {
        return false;
    }
    changing_ = false;
    if (canvas.dispatching()) {
        const auto weak = weak_from_this();
        if (weak.expired()) {
            return false;
        }
        canvas.afterDispatch(
            [weak, graph = std::move(graph), next = std::move(next), dialog, message] {
                if (auto self = weak.lock()) {
                    self->show(graph, dialog, next, message);
                }
            });
        return true;
    }
    return show(std::move(graph), dialog, std::move(next), message);
}

bool Runtime::routeEvent(const Event& e) {
    std::vector<std::shared_ptr<Delegate>> chain;
    for (const auto& delegate : delegates_ | std::views::reverse) {
        if (delegate && (chain.empty() || chain.back() != delegate)) {
            chain.push_back(delegate);
        }
    }
    if (chain.empty() && delegate_) {
        chain.push_back(delegate_);
    }
    for (auto& d : chain) {
        if (d->event(e)) {
            return true;
        }
    }
    return false;
}

bool Runtime::routeMessage(Control& c, UInt32 message) {
    std::vector<std::shared_ptr<Delegate>> chain;
    for (const auto& delegate : delegates_ | std::views::reverse) {
        if (delegate && (chain.empty() || chain.back() != delegate)) {
            chain.push_back(delegate);
        }
    }
    if (chain.empty() && delegate_) {
        chain.push_back(delegate_);
    }
    for (auto& d : chain) {
        if (auto result = d->routeMessage(c, message)) {
            return *result;
        }
    }
    if (editor_) {
        return true;
    }
    if (message > 100001 && message < 109999) {
        auto file = resources_.string ? resources_.string(Int32(message)) : std::nullopt;
        return file && open(*file);
    }
    if (message > 110000 && message < 119999) {
        if (message == 110001 || message == 110002) {
            if (canvas.dispatching()) {
                const auto weak = weak_from_this();
                if (weak.expired()) {
                    return false;
                }
                canvas.afterDispatch([weak, message] {
                    if (auto self = weak.lock()) {
                        self->pop(message);
                    }
                });
                return true;
            }
            return pop(message);
        }
        auto file = resources_.string ? resources_.string(Int32(message)) : std::nullopt;
        return file && open(*file, true, message);
    }
    if (message > 120000 && message < 129999) {
        auto script = resources_.string ? resources_.string(Int32(message)) : std::nullopt;
        return script && (console_.isFunction(*script) ? console_.execute(*script)
                                                       : console_.evaluate(*script))
                             .ok();
    }
    return true;
}

bool Runtime::save(const std::filesystem::path& path) const {
    if (graphs_.empty() || canvas.dispatching()) {
        return false;
    }
    auto* parent = editor_ ? editor_->parent() : nullptr;
    auto* host = parent ? parent->canvas() : nullptr;
    auto* focus = host ? host->focus() : nullptr;
    const bool captured = host && host->capture() == editor_.get();
    if (parent && !parent->removeObject(*editor_)) {
        return false;
    }
    const auto restore = [&] {
        if (!parent) {
            return true;
        }
        if (!parent->addObject(*editor_)) {
            return false;
        }
        if (host) {
            host->rebuildTabs();
            if (focus && !host->makeFirstResponder(focus)) {
                return false;
            }
            if (captured && !host->lockMouse(editor_.get())) {
                host->cancel();
                return false;
            }
        }
        return true;
    };
    bool result{};
    try {
        Persistent::StoreContext store(registry_);
        result = store.fileStore(path, graphs_.front()->root()) == Persistent::Error::Ok;
    } catch (...) {
        restore();
        throw;
    }
    return restore() && result;
}

bool Runtime::editing(bool enabled) {
    if (canvas.dispatching() || canvas.changingContent() ||
        (enabled && (stopped_ || !canvas.top()))) {
        return false;
    }
    if (enabled == bool(editor_)) {
        return true;
    }
    if (!enabled) {
        if (!graphs_.empty()) {
            auto& owned = graphs_.back()->objects_;
            owned.reserve(owned.size() + editor_->ownedObjectCount());
        }
        auto objects = editor_->takeObjects();
        if (!graphs_.empty()) {
            auto& owned = graphs_.back()->objects_;
            owned.reserve(owned.size() + objects.size());
            for (auto& object : objects) {
                owned.push_back(std::move(object));
            }
        }
        editor_->close();
        if (auto* parent = editor_->parent()) {
            parent->removeObject(*editor_);
        }
        editor_.reset();
        canvas.editMode = false;
        canvas.rebuildTabs();
        return true;
    }
    auto next = std::make_unique<EditControl>();
    if (!next->resize({}, canvas.top()->fields().extent) || !canvas.top()->addObject(*next)) {
        return false;
    }
    const auto weak = weak_from_this();
    next->inspect = [weak](Control* c) {
        auto self = weak.lock();
        return self && (!self->inspect || self->inspect(c));
    };
    next->destroySelection = [weak](std::span<Control*> controls) {
        auto self = weak.lock();
        if (!self || self->graphs_.empty() || !self->editor_) {
            return false;
        }
        auto graph = self->graphs_.back();
        auto& owned = graph->objects_;
        owned.reserve(owned.size() + self->editor_->ownedObjectCount());
        auto added = self->editor_->takeObjects();
        for (auto& object : added) {
            owned.push_back(std::move(object));
        }
        std::unordered_set<Persistent::Base*> retired;
        const auto collect = [&](auto&& visit, Control& c) -> bool {
            if (c.getManager() || &c == &graph->root() ||
                std::ranges::none_of(owned, [&](const auto& p) {
                    return p.get() == &c;
                })) {
                return false;
            }
            retired.insert(&c);
            for (auto* child : c.objects()) {
                if (!visit(visit, *static_cast<Control*>(child))) {
                    return false;
                }
            }
            return true;
        };
        for (auto* c : controls) {
            if (!collect(collect, *c)) {
                return false;
            }
        }
        auto* parent = controls.front()->parent();
        if (!parent || std::ranges::any_of(controls, [&](auto* c) {
                return c->parent() != parent;
            })) {
            return false;
        }
        const std::vector<sim::SimObject*> order(parent->objects().begin(),
                                                 parent->objects().end());
        std::vector<Control*> detached;
        for (auto* c : controls) {
            if (!parent->removeObject(*c)) {
                for (auto* previous : detached) {
                    parent->addObject(*previous);
                }
                for (auto* previous : order) {
                    parent->reOrder(*previous);
                }
                return false;
            }
            detached.push_back(c);
        }
        std::erase_if(owned, [&](const auto& object) {
            return retired.contains(object.get());
        });
        return true;
    };
    next->copySelection = [weak](bool cut) {
        auto self = weak.lock();
        if (!self || !self->editor_) {
            return false;
        }
        std::vector<std::byte> bytes(16U * 1024U * 1024U);
        MemRWStream stream{std::span<std::byte>(bytes)};
        if (!self->editor_->saveSelection(stream, self->registry_)) {
            return false;
        }
        bytes.resize(stream.getPosition());
        if (cut && !self->editor_->erase()) {
            return false;
        }
        self->selectionClipboard_ = std::move(bytes);
        return true;
    };
    next->pasteSelection = [weak] {
        auto self = weak.lock();
        if (!self || !self->editor_ || self->selectionClipboard_.empty()) {
            return false;
        }
        MemRWStream stream{std::span<const std::byte>(self->selectionClipboard_)};
        return self->editor_->loadSelection(stream, self->registry_, [&](Control& c) {
            return bindResources(c, self->resources_);
        });
    };
    if (!canvas.makeFirstResponder(next.get())) {
        canvas.top()->removeObject(*next);
        return false;
    }
    editor_ = std::move(next);
    canvas.editMode = true;
    canvas.rebuildTabs();
    return true;
}

Control* Runtime::findControl(std::string_view key) const {
    UInt32 id{};
    const auto parsed = std::from_chars(key.data(), key.data() + key.size(), id);
    const bool number = parsed.ec == std::errc{} && parsed.ptr == key.data() + key.size();
    const auto find = [&](Control& root) -> Control* {
        if (number) {
            if (root.fields().tag == id || (id && root.getId() == id)) {
                return &root;
            }
            if (auto* found = root.findTag(id)) {
                return found;
            }
        }
        if (root.getName() == key) {
            return &root;
        }
        return root.findNamed(key);
    };
    if (scriptRoot_) {
        return find(*scriptRoot_);
    }
    if (auto* top = canvas.top()) {
        if (auto* found = find(*top)) {
            return found;
        }
    }
    for (const auto& graph : graphs_ | std::views::reverse) {
        if (auto* found = find(graph->root())) {
            return found;
        }
    }
    return nullptr;
}

std::optional<std::string> Runtime::command(std::string_view name,
                                            std::span<const std::string> args) {
    if (stopped_ || args.size() > 64) {
        return "False";
    }
    std::size_t bytes = 0;
    for (const auto& value : args) {
        bytes += value.size();
        if (value.size() > 65536 || value.find('\0') != value.npos || bytes > 1048576) {
            return "False";
        }
    }
    // Native commands qualify the canvas. The compact spelling remains useful
    // for an embedded runtime which has no manager name or ID.
    if (!args.empty() && ((!canvas.getName().empty() && args[0] == canvas.getName()) ||
                          (canvas.getId() && args[0] == std::to_string(canvas.getId())))) {
        args = args.subspan(1);
    }
    const auto yes = [](bool value) -> std::optional<std::string> {
        return value ? "True" : "False";
    };
    const auto integer = [](std::string_view text, auto& value) {
        const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
        return r.ec == std::errc{} && r.ptr == text.data() + text.size();
    };
    const auto truth = [](std::string_view value) {
        return value == "true" || value == "True" || value == "TRUE" || value == "1";
    };
    const auto find = [&](std::string_view key) {
        return findControl(key);
    };
    if (name == "GuiLoadContentCtrl" || name == "GuiPushDialog") {
        return yes(args.size() == 1 && open(args[0], name == "GuiPushDialog"));
    }
    if (name == "GuiPopDialog") {
        if (args.size() > 1) {
            return "False";
        }
        if (canvas.dispatching()) {
            const auto weak = weak_from_this();
            if (weak.expired()) {
                return "False";
            }
            canvas.afterDispatch([weak] {
                if (auto self = weak.lock()) {
                    self->pop();
                }
            });
            return yes(graphs_.size() > 1);
        }
        return yes(pop());
    }
    if (name == "GuiSaveContentCtrl") {
        return yes(args.size() == 1 && save(args[0]));
    }
    if (name == "setCursor") {
        if (args.size() != 1 || !loadCursor) {
            return "False";
        }
        auto cursor = loadCursor(args[0]);
        if (!cursor || (!cursor->bitmap && !cursor->frames) || !std::isfinite(cursor->frameTime) ||
            cursor->frameTime <= 0) {
            return "False";
        }
        return yes(canvas.setCursor(std::move(*cursor)));
    }
    if (name == "isCursorOn") {
        return args.empty() ? yes(canvas.cursorVisible) : std::optional<std::string>("False");
    }
    if (name == "cursorOn" || name == "cursorOff") {
        if (!args.empty()) {
            return "False";
        }
        canvas.cursorEnabled = canvas.cursorVisible = name == "cursorOn";
        return "True";
    }
    if (name.starts_with("windowsMouse") || name.starts_with("windowsKeyboard")) {
        return yes(args.empty() && inputEnabled &&
                   inputEnabled(name.starts_with("windowsMouse"), name.ends_with("Enable")));
    }
    if (name == "GuiSendRootMessage") {
        UInt32 message{};
        return yes(args.size() == 1 && integer(args[0], message) && canvas.top() &&
                   canvas.top()->message(*canvas.top(), message));
    }
    if (name == "GuiNewContentCtrl" || name == "GuiEditNewControl") {
        if (args.size() != 1 || canvas.dispatching()) {
            return "False";
        }
        const auto* descriptor = registry_.find(args[0]);
        auto object = descriptor ? descriptor->create() : nullptr;
        auto* control = dynamic_cast<Control*>(object.get());
        if (!control || !bindResources(*control, resources_)) {
            return "False";
        }
        object.release();
        std::unique_ptr<Control> owned(control);
        if (name == "GuiNewContentCtrl") {
            return yes(show(Graph::create(std::move(owned))));
        }
        return yes(editing(true) && editor_->addNewControl(std::move(owned)));
    }
    if (name == "GuiEditMode") {
        return yes(args.empty() && editing(!editor_));
    }
    if (name == "GuiSetAddSet" || name == "GuiSetSelection") {
        if (args.empty() || args.size() > (name == "GuiSetSelection" ? 2U : 1U) ||
            canvas.dispatching()) {
            return "False";
        }
        auto* control = find(args[0]);
        if (!control || !editing(true)) {
            return "False";
        }
        return yes(name == "GuiSetAddSet"
                       ? editor_->addSet(*control)
                       : editor_->select(*control, args.size() == 2 && truth(args[1])));
    }
    if (name == "GuiInspect" || name == "GuiToolbar") {
        if (args.size() > (name == "GuiInspect" ? 1U : 0U) ||
            (name == "GuiInspect" ? !inspect : !toolbar) || canvas.dispatching()) {
            return "False";
        }
        const bool wasEditing = bool(editor_);
        auto restoreInteraction = canvas.interactionCheckpoint();
        auto* root = canvas.top();
        const auto rootLifetime = root ? root->lifetime() : std::weak_ptr<const void>{};
        const auto focusLifetime =
            canvas.focus() ? canvas.focus()->lifetime() : std::weak_ptr<const void>{};
        const bool hadFocus = canvas.focus() != nullptr;
        const auto captureLifetime =
            canvas.capture() ? canvas.capture()->lifetime() : std::weak_ptr<const void>{};
        const bool hadCapture = canvas.capture() != nullptr;
        const auto hoverLifetime =
            canvas.hover() ? canvas.hover()->lifetime() : std::weak_ptr<const void>{};
        const bool hadHover = canvas.hover() != nullptr;
        if (!editing(true)) {
            return "False";
        }
        bool accepted = false;
        try {
            const auto selected = editor_->selection();
            accepted = name == "GuiInspect"
                           ? inspect(selected.size() == 1 ? selected.front() : canvas.top())
                           : toolbar(*editor_);
        } catch (...) {
            accepted = false;
        }
        if (!accepted && !wasEditing) {
            editing(false);
            if (!rootLifetime.expired() && canvas.top() == root &&
                (!hadFocus || !focusLifetime.expired()) &&
                (!hadCapture || !captureLifetime.expired()) &&
                (!hadHover || !hoverLifetime.expired())) {
                restoreInteraction();
            } else {
                canvas.cancel();
            }
        }
        return yes(accepted);
    }
    if (name == "GuiSaveSelection") {
        if (args.size() != 1 || !editor_ || !saveFile) {
            return "False";
        }
        std::vector<std::byte> data(16U * 1024U * 1024U);
        MemRWStream stream{std::span<std::byte>(data)};
        return yes(editor_->saveSelection(stream, registry_) &&
                   saveFile(args[0], std::span<const std::byte>(data).first(stream.getPosition())));
    }
    if (name == "GuiLoadSelection") {
        if (args.size() != 1 || !loadFile || canvas.dispatching() || !editing(true)) {
            return "False";
        }
        auto data = loadFile(args[0]);
        if (!data) {
            return "False";
        }
        MemRWStream stream{std::span<const std::byte>(*data)};
        return yes(editor_->loadSelection(stream, registry_, [&](Control& c) {
            return bindResources(c, resources_);
        }));
    }
    if (name == "GuiJustify") {
        UInt32 mode{};
        constexpr std::array modes{EditControl::Align::Left,
                                   EditControl::Align::HorizontalCenter,
                                   EditControl::Align::Right,
                                   EditControl::Align::Top,
                                   EditControl::Align::Bottom,
                                   EditControl::Align::VerticalSpacing,
                                   EditControl::Align::HorizontalSpacing};
        return yes(args.size() == 1 && integer(args[0], mode) && mode < modes.size() && editor_ &&
                   editor_->align(modes[mode]));
    }
    if (name == "GuiBringToFront" || name == "GuiSendToBack" || name == "GuiUndo" ||
        name == "GuiRedo" || name == "GuiDeleteSelection") {
        if (!args.empty() || !editor_ || canvas.dispatching()) {
            return "False";
        }
        if (name == "GuiBringToFront") {
            return yes(editor_->bringToFront());
        }
        if (name == "GuiSendToBack") {
            return yes(editor_->sendToBack());
        }
        if (name == "GuiUndo") {
            return yes(editor_->undo());
        }
        if (name == "GuiRedo") {
            return yes(editor_->redo());
        }
        return yes(editor_->erase());
    }
    if (args.empty()) {
        return "False";
    }
    auto* control = find(args[0]);
    if (!control) {
        return "False";
    }
    if (name == "Control::setText" || name == "Control::setValue") {
        return yes(args.size() == 2 && setValue(*control, args[1]));
    }
    if (name == "Control::getText" || name == "Control::getValue") {
        return args.size() == 1 ? std::optional(scriptValue(*control))
                                : std::optional<std::string>("False");
    }
    if (name == "Control::setVisible") {
        if (args.size() != 2) {
            return "False";
        }
        control->visible(truth(args[1]));
        return "True";
    }
    if (name == "Control::getVisible") {
        return yes(args.size() == 1 && control->visible());
    }
    if (name == "Control::setActive" || name == "Control::getActive" ||
        name == "Control::performClick") {
        auto* active = dynamic_cast<Active*>(control);
        if (!active) {
            return "False";
        }
        if (name == "Control::getActive") {
            return yes(args.size() == 1 && active->active);
        }
        if (name == "Control::setActive") {
            if (args.size() != 2) {
                return "False";
            }
            active->active = truth(args[1]);
            return "True";
        }
        if (args.size() != 1) {
            return "False";
        }
        return yes(canvas.performClick(*active));
    }
    if (auto* list = dynamic_cast<TextList*>(control)) {
        if (name == "TextList::Clear") {
            if (args.size() != 1) {
                return "False";
            }
            list->clear();
            return "True";
        }
        if (name == "TextList::AddLine") {
            return yes(args.size() == 2 && list->insert({0, args[1]}));
        }
    }
    return "False";
}

bool Runtime::action(Control& control, UInt32 message, std::string_view command) {
    if (stopped_) {
        return false;
    }
    // A command or onAction callback may replace the screen and destroy its
    // sender synchronously (movie completion also runs outside dispatch).
    // Own script inputs and observe the sender before crossing either callback.
    const auto variable = control.fields().variable;
    const auto script = std::string(command);
    const auto life = control.lifetime();
    const bool had = !variable.empty() && console_.hasVariable(variable);
    const auto old = variable.empty() ? std::string{} : console_.getVariable(variable);
    const auto rollback = [&] {
        if (!variable.empty()) {
            if (had) {
                console_.setVariable(variable, old);
            } else {
                console_.removeVariable(variable);
            }
        }
    };
    try {
        if (!variable.empty()) {
            console_.setVariable(variable, scriptValue(control));
        }
        if (!script.empty() && !console_.evaluate(script).ok()) {
            rollback();
            return false;
        }
        if (life.expired()) {
            return true;
        }
        const auto name = control.getName();
        if (!name.empty()) {
            const auto function = std::string(name) + "::onAction";
            if (console_.isFunction(function) && !console_.execute(function).ok()) {
                rollback();
                return false;
            }
        }
        if (life.expired()) {
            return true;
        }
        if (message && !control.message(control, message)) {
            rollback();
            return false;
        }
    } catch (...) {
        rollback();
        return false;
    }
    return true;
}

bool Runtime::synchronizeVariables() {
    if (stopped_) {
        return false;
    }
    std::vector<std::pair<Control*, std::function<void()>>> undo;
    const auto visit = [&](auto&& self, Control& c) -> bool {
        if (!c.fields().variable.empty() && console_.hasVariable(c.fields().variable)) {
            const auto value = console_.getVariable(c.fields().variable);
            auto* text = dynamic_cast<Text*>(&c);
            if (!text || text->text != value) {
                undo.emplace_back(&c, c.checkpoint());
                if (!setValue(c, value)) {
                    return false;
                }
            }
        }
        for (auto* o : c.objects()) {
            if (!self(self, *static_cast<Control*>(o))) {
                return false;
            }
        }
        return true;
    };
    try {
        for (auto& graph : graphs_) {
            if (!visit(visit, graph->root())) {
                for (auto& entry : undo) {
                    entry.second();
                }
                return false;
            }
        }
    } catch (...) {
        for (auto& entry : undo) {
            entry.second();
        }
        return false;
    }
    return true;
}

void Runtime::install() {
    if (stopped_) {
        throw std::logic_error("GUI runtime is stopped");
    }
    if (installed_) {
        return;
    }
    for (auto name : names) {
        if (console_.isFunction(name)) {
            throw std::runtime_error("GUI command namespace in use");
        }
    }
    const auto weak = weak_from_this();
    if (weak.expired()) {
        throw std::logic_error("GUI runtime requires shared ownership");
    }
    std::vector<std::shared_ptr<void>> commands;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < names.size();
                  })) {
        commands.push_back(console_.scopedCommand(
            Int32(i),
            names[i],
            [weak](Console&, Int32 id, std::span<const std::string> args)
                -> std::optional<std::string> {
                auto self = weak.lock();
                if (!self || args.empty()) {
                    return "False";
                }
                try {
                    return self->command(names[std::size_t(id)], args.subspan(1));
                } catch (...) {
                    return "False";
                }
            }));
    }
    commands_ = std::move(commands);
    installed_ = true;
}

void Runtime::uninstall() {
    commands_.clear();
    installed_ = false;
}

void Runtime::stop() {
    if (stopped_) {
        return;
    }
    uninstall();
    canvas.cancel();
    editing(false);
    stopped_ = true;
}
} // namespace opent1::gui
