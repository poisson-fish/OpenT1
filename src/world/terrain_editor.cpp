#include <opent1/world/terrain_editor.hpp>
#include <ranges>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <atomic>

namespace opent1::world {
namespace {
constexpr std::array<std::string_view, 29> actionNames{"adjustControlPoint",
                                                       "adjustCorner",
                                                       "adjustHeight",
                                                       "clearFlags",
                                                       "clearPin",
                                                       "copy",
                                                       "deselect",
                                                       "depress",
                                                       "elevate",
                                                       "getAvgHeight",
                                                       "lowerHeight",
                                                       "noise",
                                                       "paste",
                                                       "pin",
                                                       "raiseHeight",
                                                       "redo",
                                                       "relight",
                                                       "rotateLeft",
                                                       "rotateRight",
                                                       "scale",
                                                       "select",
                                                       "setControlHeight",
                                                       "setCornerHeight",
                                                       "setFlags",
                                                       "setHeight",
                                                       "setMaterial",
                                                       "setTerrainType",
                                                       "smooth",
                                                       "undo"};

bool pointValid(const TerrainDocument& d, Point2I p) {
    const auto width = 1U << (d.grid.detailCount - 1);
    return p.x >= 0 && p.y >= 0 && Int64(p.x) < Int64(d.grid.size.x) * width &&
           Int64(p.y) < Int64(d.grid.size.y) * width;
}

struct Address {
    std::size_t block{}, height{}, material{};
};

std::optional<Address> address(const TerrainDocument& d, Point2I p) {
    if (!pointValid(d, p)) {
        return {};
    }
    const auto width = 1U << (d.grid.detailCount - 1), x = UInt32(p.x), y = UInt32(p.y);
    const auto id = d.grid.blockMap[std::size_t(y / width) * d.grid.size.x + x / width];
    const auto at = std::ranges::find(d.grid.blocks, id, &TerrainGridBlockEntry::id);
    if (at == d.grid.blocks.end()) {
        return {};
    }
    return Address{std::size_t(at - d.grid.blocks.begin()),
                   std::size_t(y % width) * (width + 1) + x % width,
                   std::size_t(y % width) * width + x % width};
}

std::optional<TerrainSelectionInfo> info(const TerrainDocument& d, Point2I p) {
    const auto a = address(d, p);
    if (!a) {
        return {};
    }
    return TerrainSelectionInfo{
        p, d.blocks[a->block].heights[a->height], d.blocks[a->block].materials[a->material]};
}

bool height(TerrainDocument& d, Point2I p, Flt32 h) {
    if (!pointValid(d, p) || !std::isfinite(h) || std::abs(h) > 1e7f) {
        return false;
    }
    const auto width = 1U << (d.grid.detailCount - 1), x = UInt32(p.x), y = UInt32(p.y),
               bx = x / width, by = y / width;
    bool changed = false;
    for (auto j : std::views::iota(UInt32{0}, UInt32{2})) {
        for (auto i : std::views::iota(UInt32{0}, UInt32{2})) {
            if ((i && (!bx || x % width)) || (j && (!by || y % width))) {
                continue;
            }
            const auto id = d.grid.blockMap[std::size_t(by - j) * d.grid.size.x + bx - i];
            const auto at = std::ranges::find(d.grid.blocks, id, &TerrainGridBlockEntry::id);
            if (at == d.grid.blocks.end()) {
                continue;
            }
            auto& b = d.blocks[std::size_t(at - d.grid.blocks.begin())];
            b.heights[std::size_t(y - (by - j) * width) * (width + 1) + x - (bx - i) * width] = h;
            changed = true;
        }
    }
    return changed;
}

bool material(TerrainDocument& d, Point2I p, TerrainGridMaterial m) {
    const auto a = address(d, p);
    if (!a) {
        return false;
    }
    d.blocks[a->block].materials[a->material] = m;
    return true;
}

bool contains(std::span<const Point2I> list, Point2I p) {
    return std::ranges::find_if(list, [&](auto q) {
               return q.x == p.x && q.y == p.y;
           }) != list.end();
}

std::string lower(std::string_view value) {
    std::string s(value);
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c + 32);
        }
    }
    return s;
}

bool selectionName(std::string_view s) {
    return !s.empty() && s.size() < 255 && s.find('\0') == s.npos;
}

std::optional<double> number(std::string_view s) {
    double n{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size() || !std::isfinite(n) || std::abs(n) > 1e7) {
        return {};
    }
    return n;
}

std::optional<double> boolean(std::string_view s) {
    const auto v = lower(s);
    if (v == "true" || v == "t") {
        return 1.;
    }
    if (v == "false" || v == "f") {
        return 0.;
    }
    return number(s);
}

std::optional<UInt8> flags(std::span<const std::string> args) {
    if (args.empty()) {
        return {};
    }
    UInt8 value{};
    for (const auto& text : args) {
        const auto s = lower(text);
        if (const auto n = number(s); n && *n >= 0 && *n <= 255 && *n == std::floor(*n)) {
            value |= UInt8(*n);
        } else if (s == "rotate") {
            value |= 1;
        } else if (s == "flipx") {
            value |= 2;
        } else if (s == "flipy") {
            value |= 4;
        } else if (s == "edit") {
            value |= 64;
        } else if (s == "corner") {
            value |= 128;
        } else if (s == "all") {
            value = 255;
        } else if (s.size() == 6 && s.starts_with("empty") && s[5] >= '0' && s[5] <= '7') {
            value |= UInt8((s[5] - '0') << 3);
        } else {
            return {};
        }
    }
    return value;
}

std::optional<std::vector<std::byte>> readFile(const std::filesystem::path& p,
                                               UInt32 maximum = 256U * 1024U * 1024U) {
    FileRStream f(p);
    if (f.getStatus() != STRM_OK || f.getSize() > maximum) {
        return {};
    }
    std::vector<std::byte> bytes(f.getSize());
    if (!f.readBytes(bytes)) {
        return {};
    }
    return bytes;
}

using OutputFile = std::pair<std::filesystem::path, std::vector<std::byte>>;

bool writeFiles(std::span<const OutputFile> files) {
    if (files.empty()) {
        return true;
    }
    if (files.size() > 64) {
        return false;
    }

    struct Entry {
        std::filesystem::path path, temp, backup;
        bool old{}, published{};
    };

    std::vector<Entry> staged;
    static std::atomic<UInt64> serial{};
    std::size_t total{};
    std::set<std::string> paths;
    const auto rollback = [&] {
        for (const auto& entry : staged | std::views::reverse) {
            std::error_code ec;
            if (entry.published) {
                std::filesystem::remove(entry.path, ec);
            }
            if (entry.old) {
                std::filesystem::rename(entry.backup, entry.path, ec);
            }
            std::filesystem::remove(entry.temp, ec);
        }
    };
    for (const auto& [path, bytes] : files) {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(path, ec).lexically_normal();
        total += bytes.size();
        if (ec || total > 256U * 1024U * 1024U ||
            !paths.insert(lower(absolute.generic_string())).second ||
            (std::filesystem::exists(path, ec) && !std::filesystem::is_regular_file(path, ec))) {
            rollback();
            return false;
        }
        Entry e;
        e.path = path;
        e.temp = path;
        e.temp += std::format(".opent1-{}.tmp", ++serial);
        e.backup = e.temp;
        e.backup += ".bak";
        if (std::filesystem::exists(e.temp, ec) || std::filesystem::exists(e.backup, ec)) {
            rollback();
            return false;
        }
        staged.push_back(e);
        FileWStream out(e.temp);
        if (out.getStatus() != STRM_OK || !out.writeBytes(bytes) || !out.flush()) {
            out.close();
            rollback();
            return false;
        }
        out.close();
    }
    for (auto& e : staged) {
        std::error_code ec;
        if (std::filesystem::exists(e.path, ec)) {
            std::filesystem::rename(e.path, e.backup, ec);
            if (ec) {
                rollback();
                return false;
            }
            e.old = true;
        }
        std::filesystem::rename(e.temp, e.path, ec);
        if (ec) {
            rollback();
            return false;
        }
        e.published = true;
    }
    for (auto& e : staged) {
        if (e.old) {
            std::error_code ec;
            std::filesystem::remove(e.backup, ec);
        }
    }
    return true;
}
}

std::span<const std::string_view> TerrainEditor::actions() {
    return actionNames;
}

bool writeTerrainEditorFiles(std::span<const TerrainOutputFile> files) {
    return writeFiles(files);
}

bool TerrainEditor::materialResources(std::string_view name,
                                      std::span<const std::byte> bytes,
                                      std::optional<std::span<const std::byte>> textures) {
    if (working_ || !snapshot() || name.empty() || name.size() > 255 ||
        name.find('\0') != name.npos || bytes.size() > MaxTsFileBytes) {
        return false;
    }
    MemRWStream input(bytes);
    auto list = TsMaterialList::load(input, UInt32(bytes.size()));
    if (!list) {
        return false;
    }
    auto staged = materialLists_;
    if (staged.size() >= 64 && !staged.contains(name)) {
        return false;
    }
    staged[std::string(name)] = std::move(*list);
    auto next = mapper;
    if (textures) {
        auto table = LandscapeTextureTable::load(*textures);
        if (!table ||
            staged.at(std::string(name)).materialsPerDetail != Int32(table->corners.size()) ||
            !next.setTextures(*table)) {
            return false;
        }
    }
    if (!history_.edit([&](auto& d) {
            d.grid.materialList = name;
            return true;
        })) {
        return false;
    }
    materialLists_ = std::move(staged);
    mapper = std::move(next);
    return true;
}

bool TerrainEditor::attach(TerrainDocument d) {
    if (!history_.attach(std::move(d))) {
        return false;
    }
    selection_.clear();
    clipboard_.clear();
    named_.clear();
    working_.reset();
    gestureAction_.clear();
    floating_ = false;
    materialLists_.clear();
    return true;
}

void TerrainEditor::detach() {
    history_.detach();
    selection_.clear();
    clipboard_.clear();
    named_.clear();
    working_.reset();
    gestureAction_.clear();
    floating_ = false;
    materialLists_.clear();
}

bool TerrainEditor::open(std::span<const std::byte> bytes) {
    auto d = TerrainDocument::load(bytes);
    return d && attach(std::move(*d));
}

bool TerrainEditor::save(const std::filesystem::path& p) const {
    auto d = snapshot();
    return d && !working_ && d->save(p);
}

bool TerrainEditor::settings(const TerrainEditorSettings& s) {
    if (!std::isfinite(s.height) || !std::isfinite(s.adjust) || !std::isfinite(s.scale) ||
        !std::isfinite(s.smooth) || !std::isfinite(s.mouseAdjust) || std::abs(s.height) > 1e7f ||
        std::abs(s.adjust) > 1e6f || std::abs(s.scale) > 1e6f || s.smooth < 0 || s.smooth > 1 ||
        std::abs(s.mouseAdjust) > 1000 || s.brushDetail > 10 || s.pinDetail < 1 ||
        s.pinDetail >= 10 || s.pinMax < s.pinDetail || s.pinMax > 11 ||
        s.leftAction >= actionNames.size() || s.rightAction >= actionNames.size() ||
        (s.paste & ~3U)) {
        return false;
    }
    settings_ = s;
    return true;
}

std::function<void()> TerrainEditor::checkpoint() {
    auto state = std::make_shared<TerrainEditor>(*this);
    if (working_) {
        state->working_ = std::make_shared<TerrainDocument>(*working_);
    }
    return [this, state = std::move(state)] {
        const auto saved = state->history_.snapshot();
        if (saved != history_.snapshot() && saved && state->history_.publish &&
            !state->history_.publish(*saved)) {
            return;
        }
        *this = *state;
        if (state->working_) {
            working_ = std::make_shared<TerrainDocument>(*state->working_);
        }
    };
}

bool TerrainEditor::select(std::span<const Point2I> points, bool add) {
    const auto d = snapshot();
    if (!d || points.size() > 1'048'576) {
        return false;
    }
    auto next = add ? selection_ : std::vector<Point2I>{};
    std::set<std::pair<Int32, Int32>> used;
    for (auto p : next) {
        used.emplace(p.x, p.y);
    }
    for (auto p : points) {
        if (!pointValid(*d, p)) {
            return false;
        }
        if (used.emplace(p.x, p.y).second) {
            next.push_back(p);
        }
    }
    if (next.size() > 1'048'576) {
        return false;
    }
    selection_ = std::move(next);
    return true;
}

bool TerrainEditor::brush(Point2I p) {
    auto d = snapshot();
    if (!d || !pointValid(*d, p)) {
        return false;
    }
    const auto dim = Int32(1U << settings_.brushDetail);
    if (settings_.snap) {
        p.x &= ~(dim - 1);
        p.y &= ~(dim - 1);
    }
    std::vector<Point2I> points;
    for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                      return y < dim;
                  })) {
        for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                          return x < dim;
                      })) {
            const Point2I q{p.x + x, p.y + y};
            if (pointValid(*d, q)) {
                points.push_back(q);
            }
        }
    }
    if (!select(points)) {
        return false;
    }
    brush_ = p;
    return true;
}

bool TerrainEditor::region(Point2I first, Point2I last, bool add) {
    const auto d = snapshot();
    if (!d || !pointValid(*d, first) || !pointValid(*d, last)) {
        return false;
    }
    const auto left = std::min(first.x, last.x), right = std::max(first.x, last.x),
               top = std::min(first.y, last.y), bottom = std::max(first.y, last.y);
    if ((Int64(right) - left + 1) * (Int64(bottom) - top + 1) > 1'048'576) {
        return false;
    }
    std::vector<Point2I> p;
    for (auto y : std::views::iota(top) | std::views::take_while([&](auto y) {
                      return y <= bottom;
                  })) {
        for (auto x : std::views::iota(left) | std::views::take_while([&](auto x) {
                          return x <= right;
                      })) {
            p.push_back({x, y});
        }
    }
    return select(p, add);
}

bool TerrainEditor::undo() {
    return !working_ && history_.undo();
}

bool TerrainEditor::redo() {
    return !working_ && history_.redo();
}

bool TerrainEditor::floatSelection() {
    if (!action("copy") || clipboard_.empty()) {
        return false;
    }
    floating_ = true;
    return true;
}

bool TerrainEditor::floatPaste() {
    if (clipboard_.empty() || working_) {
        return false;
    }
    floating_ = true;
    return true;
}

bool TerrainEditor::nameSelection(std::string_view name) {
    if (!selectionName(name) || named_.size() >= 1024 || named_.contains(name)) {
        return false;
    }
    named_.emplace(std::string(name), selection_);
    return true;
}

bool TerrainEditor::chooseSelection(std::string_view name) {
    const auto at = named_.find(name);
    return at != named_.end() && select(at->second);
}

bool TerrainEditor::removeSelection(std::string_view name) {
    const auto at = named_.find(name);
    if (at == named_.end()) {
        return false;
    }
    named_.erase(at);
    return true;
}

std::optional<std::vector<std::byte>> TerrainEditor::saveSelection() const {
    const auto d = snapshot();
    if (!d) {
        return {};
    }
    std::vector<std::byte> bytes(4 + selection_.size() * 16);
    MemRWStream f{std::span<std::byte>(bytes)};
    if (!f.write(UInt32(selection_.size()))) {
        return {};
    }
    for (auto p : selection_) {
        const auto i = info(*d, p);
        if (!i || !f.write(p.x) || !f.write(p.y) || !f.write(i->height) ||
            !f.write(i->material.flags) || !f.write(i->material.index) || !f.write(UInt16{})) {
            return {};
        }
    }
    return bytes;
}

bool TerrainEditor::loadSelection(std::span<const std::byte> bytes) {
    if (bytes.size() < 4 || bytes.size() > 16U * 1024U * 1024U + 4) {
        return false;
    }
    MemRWStream f(bytes);
    UInt32 count{};
    if (!f.read(count) || count > 1'048'576 || bytes.size() != 4 + std::size_t(count) * 16) {
        return false;
    }
    std::vector<Point2I> points;
    for ([[maybe_unused]] auto j :
         std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 j) {
             return j < count;
         })) {
        TerrainSelectionInfo i;
        UInt16 padding{};
        if (!f.read(i.point.x) || !f.read(i.point.y) || !f.read(i.height) ||
            !std::isfinite(i.height) || !f.read(i.material.flags) || !f.read(i.material.index) ||
            !f.read(padding)) {
            return false;
        }
        points.push_back(i.point);
    }
    return select(points);
}

std::optional<std::vector<std::byte>> TerrainEditor::saveNamedSelections() const {
    const auto d = snapshot();
    if (!d || working_) {
        return {};
    }
    std::size_t bytesCount = 4;
    for (const auto& [name, points] : named_) {
        if (!selectionName(name) || points.size() > 1'048'576) {
            return {};
        }
        bytesCount += 8 + name.size() + points.size() * 8;
    }
    if (bytesCount > 64U * 1024U * 1024U) {
        return {};
    }
    std::vector<std::byte> bytes(bytesCount);
    MemRWStream f{std::span<std::byte>(bytes)};
    if (!f.write(UInt32(named_.size()))) {
        return {};
    }
    for (const auto& [name, points] : named_) {
        if (!f.write(UInt32(name.size())) || !f.writeBytes(std::as_bytes(std::span(name))) ||
            !f.write(UInt32(points.size()))) {
            return {};
        }
        for (auto p : points) {
            if (!pointValid(*d, p) || !f.write(p.x) || !f.write(p.y)) {
                return {};
            }
        }
    }
    return bytes;
}

bool TerrainEditor::loadNamedSelections(std::span<const std::byte> bytes) {
    const auto d = snapshot();
    if (!d || working_ || bytes.size() < 4 || bytes.size() > 64U * 1024U * 1024U) {
        return false;
    }
    MemRWStream f(bytes);
    UInt32 count{};
    if (!f.read(count) || count > 1024) {
        return false;
    }
    std::map<std::string, std::vector<Point2I>, std::less<>> staged;
    for ([[maybe_unused]] auto s :
         std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 s) {
             return s < count;
         })) {
        UInt32 len{}, n{};
        if (!f.read(len) || !len || len > 254) {
            return false;
        }
        std::string name(len, '\0');
        if (!f.readBytes(std::as_writable_bytes(std::span(name))) || !selectionName(name) ||
            staged.contains(name)) {
            return false;
        }
        if (!f.read(n) || n > 1'048'576) {
            return false;
        }
        std::vector<Point2I> points;
        std::set<std::pair<Int32, Int32>> used;
        for ([[maybe_unused]] auto j :
             std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 j) {
                 return j < n;
             })) {
            Point2I p{};
            if (!f.read(p.x) || !f.read(p.y) || !pointValid(*d, p)) {
                return false;
            }
            if (used.emplace(p.x, p.y).second) {
                points.push_back(p);
            }
        }
        staged.emplace(std::move(name), std::move(points));
    }
    if (std::size_t(f.getPosition()) != bytes.size()) {
        return false;
    }
    named_ = std::move(staged);
    return true;
}

bool TerrainEditor::mutate(TerrainDocument& d,
                           std::string_view action,
                           std::span<const Point2I> selected,
                           Int32 delta) {
    const auto a = lower(action);
    if (selected.empty()) {
        return false;
    }
    if (a == "noise") {
        return true;
    } // original empty implementation
    if (a == "relight") {
        return relight && relight(d, false);
    }
    if (a == "setterraintype") {
        std::vector<LandscapeCorner> corners;
        for (auto p : selected) {
            corners.push_back({p, settings_.terrainType});
        }
        return mapper.apply(d, corners);
    }
    if (a == "paste") {
        if (clipboard_.empty()) {
            return false;
        }
        for (const auto& i : clipboard_) {
            const Point2I p{brush_.x + i.point.x, brush_.y + i.point.y};
            if (!pointValid(d, p)) {
                continue;
            }
            if ((settings_.paste & 1) && !material(d, p, i.material)) {
                return false;
            }
            if ((settings_.paste & 2) && !height(d, p, i.height)) {
                return false;
            }
        }
        return true;
    }
    Int32 left = selected[0].x, right = left, top = selected[0].y, bottom = top;
    for (auto p : selected) {
        left = std::min(left, p.x);
        right = std::max(right, p.x);
        top = std::min(top, p.y);
        bottom = std::max(bottom, p.y);
    }
    const auto cx = Flt32(left + right) / 2, cy = Flt32(top + bottom) / 2,
               rx = Flt32(right - left) / 2, ry = Flt32(bottom - top) / 2,
               radius = std::sqrt(rx * rx + ry * ry);
    const auto adjustControl = [&](Point2I brush, Flt32 target, bool upper, bool leftSide) -> bool {
        const auto dim = Int32(1U << settings_.brushDetail), controlX = leftSide ? 0 : dim - 1,
                   controlY = upper ? 0 : dim - 1;
        const auto initial = info(d, {brush.x + controlX, brush.y + controlY});
        if (!initial) {
            return true;
        }
        for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                          return y < dim;
                      })) {
            for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                              return x < dim;
                          })) {
                const Point2I p{brush.x + x, brush.y + y};
                const auto old = info(d, p);
                if (!old) {
                    continue;
                }
                if (x == controlX && y == controlY) {
                    if (!height(d, p, target)) {
                        return false;
                    }
                    continue;
                }
                float ex{}, ey{};
                if (x == controlX) {
                    ex = Flt32(controlX);
                    ey = Flt32(upper ? dim - 1 : 0);
                } else if (y == controlY) {
                    ex = Flt32(leftSide ? dim - 1 : 0);
                    ey = Flt32(controlY);
                } else {
                    const auto slope =
                        double(std::abs(Flt32(y - controlY))) / std::abs(Flt32(x - controlX));
                    if (slope <= 1) {
                        ex = Flt32(leftSide ? dim - 1 : 0);
                        ey = Flt32(upper ? slope * (dim - 1) : (dim - 1) - slope * (dim - 1));
                    } else {
                        ex = Flt32(leftSide ? (dim - 1) / slope : (dim - 1) - (dim - 1) / slope);
                        ey = Flt32(upper ? dim - 1 : 0);
                    }
                }
                const double distance = std::sqrt(Flt32((controlX - x) * (controlX - x) +
                                                        (controlY - y) * (controlY - y))),
                             end = std::sqrt((ex - controlX) * (ex - controlX) +
                                             (ey - controlY) * (ey - controlY));
                const float step = Flt32((target - initial->height) / end);
                const auto h = Flt32(old->height + (target - initial->height) - step * distance);
                if (!height(d, p, h)) {
                    return false;
                }
            }
        }
        return true;
    };
    if (a == "setcornerheight" || a == "setcontrolheight" || a == "adjustcorner") {
        if (!settings_.snap) {
            return true;
        }
        const auto dim = Int32(1U << settings_.brushDetail);
        const auto grabbed = info(d, grab_);
        if (a == "adjustcorner" && !grabbed) {
            return false;
        }
        const auto target = a == "adjustcorner"
                                ? grabbed->height - Flt32(delta) * settings_.mouseAdjust
                                : settings_.height;
        for (auto y : std::views::iota(Int32{-1}) | std::views::take_while([&](Int32 y) {
                          return y <= 0;
                      })) {
            for (auto x : std::views::iota(Int32{-1}) | std::views::take_while([&](Int32 x) {
                              return x <= 0;
                          })) {
                const Point2I p{brush_.x + x * dim, brush_.y + y * dim};
                if (pointValid(d, p) && !adjustControl(p, target, y == 0, x == 0)) {
                    return false;
                }
            }
        }
        return true;
    }
    if (a == "adjustcontrolpoint") {
        if (!settings_.snap) {
            return true;
        }
        const auto dim = Int32(1U << settings_.brushDetail);
        const auto change = -Flt32(delta) * settings_.mouseAdjust;
        const auto apply = [&](Point2I p, float h) {
            const auto old = info(d, p);
            return !old || height(d, p, old->height + h);
        };
        if (!apply(grab_, change)) {
            return false;
        }
        for (auto i : std::views::iota(Int32{1}) | std::views::take_while([&](Int32 i) {
                          return i < dim;
                      })) {
            const auto step = Flt32(dim - i) / Flt32(dim) * change;
            for (auto x :
                 std::views::iota(static_cast<Int32>(-i)) | std::views::take_while([&](Int32 x) {
                     return x <= i;
                 })) {
                for (Int32 s : {-1, 1}) {
                    if (!apply({grab_.x + x, grab_.y + s * i}, step)) {
                        return false;
                    }
                }
            }
            for (auto y : std::views::iota(static_cast<Int32>(-i + 1)) |
                              std::views::take_while([&](Int32 y) {
                                  return y < i;
                              })) {
                for (Int32 s : {-1, 1}) {
                    if (!apply({grab_.x + s * i, grab_.y + y}, step)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    if (!settings_.brushDetail && (a == "elevate" || a == "depress") &&
        !mutate(d, a == "elevate" ? "raiseHeight" : "lowerHeight", selected)) {
        return false;
    }
    for (auto p : selected) {
        auto old = info(d, p);
        if (!old) {
            continue;
        }
        auto h = old->height;
        auto m = old->material;
        if (a == "setheight") {
            h = settings_.height;
        } else if (a == "raiseheight" || a == "lowerheight" || a == "adjustheight") {
            h += a == "adjustheight"  ? -Flt32(delta) * settings_.mouseAdjust
                 : a == "raiseheight" ? settings_.adjust
                                      : -settings_.adjust;
        } else if (a == "scale") {
            h *= settings_.scale;
        } else if (a == "elevate" || a == "depress") {
            const auto dx = Flt32(p.x) - cx, dy = Flt32(p.y) - cy;
            h += (a == "elevate" ? settings_.adjust : -settings_.adjust) *
                 (radius - std::sqrt(dx * dx + dy * dy));
        } else if (a == "smooth") {
            if (!settings_.brushDetail || radius == 0) {
                continue;
            }
            Flt32 sum{};
            UInt32 count{};
            for (auto q : std::array<Point2I, 4>{
                     {{p.x - 1, p.y}, {p.x + 1, p.y}, {p.x, p.y - 1}, {p.x, p.y + 1}}}) {
                const auto neighbor = info(d, q);
                if (neighbor) {
                    sum += neighbor->height;
                    ++count;
                }
            }
            if (!count) {
                continue;
            }
            sum /= Flt32(count);
            const auto increment = (sum - h) / (10 - settings_.smooth * 9.999f),
                       dx = Flt32(p.x) - cx, dy = Flt32(p.y) - cy;
            auto change = increment - (increment / 2) * std::sqrt(dx * dx + dy * dy) / radius;
            if (std::abs(change) > std::abs(sum - h)) {
                change = sum - h;
            }
            h += change;
        } else if (a == "setmaterial") {
            m.index = settings_.material;
            m.flags |= 64;
        } else if (a == "setflags") {
            m.flags |= settings_.flags;
        } else if (a == "clearflags") {
            m.flags &= UInt8(~settings_.flags);
        } else if (a == "rotateleft" || a == "rotateright") {
            const auto f = m.flags;
            m.flags |= 7;
            if (a == "rotateright") {
                if (!(f & 1)) {
                    if (!(f & 2)) {
                        m.flags &= UInt8(0xf9);
                    }
                } else {
                    if (!(f & 2)) {
                        m.flags &= UInt8(0xfe);
                    } else {
                        m.flags &= UInt8(0xf8);
                    }
                }
            } else {
                if (!(f & 1)) {
                    if (f & 2) {
                        m.flags &= UInt8(0xf9);
                    }
                } else {
                    if (!(f & 2)) {
                        m.flags &= UInt8(0xf8);
                    } else {
                        m.flags &= UInt8(0xfe);
                    }
                }
            }
        } else if (a == "pin" || a == "clearpin") {
            const auto at = address(d, p);
            const auto width = 1U << (d.grid.detailCount - 1), detail = settings_.pinDetail;
            const auto max = std::min(settings_.pinMax, UInt32(d.grid.detailCount));
            if (!at || !pinTerrainSquare(d.blocks[at->block],
                                         detail,
                                         {Int32((UInt32(p.x) % width) >> detail),
                                          Int32((UInt32(p.y) % width) >> detail)},
                                         max,
                                         a == "pin")) {
                return false;
            }
            continue;
        } else {
            return false;
        }
        if (!height(d, p, h) || !material(d, p, m)) {
            return false;
        }
    }
    if (settings_.feather &&
        (a == "setheight" || a == "raiseheight" || a == "lowerheight" || a == "adjustheight")) {
        if (settings_.snap && settings_.brushDetail) {
            const auto dim = Int32(1U << settings_.brushDetail);
            const bool raise = a != "setheight";
            const auto amount = a == "setheight"      ? settings_.height
                                : a == "lowerheight"  ? -settings_.adjust
                                : a == "adjustheight" ? -Flt32(delta) * settings_.mouseAdjust
                                                      : settings_.adjust;
            for (auto n : std::views::iota(Int32{-1}) | std::views::take_while([&](Int32 n) {
                              return n <= 1;
                          })) {
                for (auto m : std::views::iota(Int32{-1}) | std::views::take_while([&](Int32 m) {
                                  return m <= 1;
                              })) {
                    if (!m && !n) {
                        continue;
                    }
                    const Point2I p{brush_.x + m * dim, brush_.y + n * dim};
                    if (!pointValid(d, p)) {
                        continue;
                    }
                    if (!m || !n) {
                        const UInt32 side = !m ? (n == -1 ? 4U : 1U) : (m == -1 ? 2U : 8U);
                        for (auto i :
                             std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                                 return i < dim;
                             })) {
                            Point2I dest{};
                            if (side == 2) {
                                dest = {p.x + dim - 1, p.y + i};
                            } else if (side == 4) {
                                dest = {p.x + i, p.y + dim - 1};
                            } else if (side == 8) {
                                dest = {p.x, p.y + i};
                            } else {
                                dest = {p.x + i, p.y};
                            }
                            const auto old = info(d, dest);
                            if (!old) {
                                continue;
                            }
                            const auto step = raise ? amount / Flt32(dim - 1)
                                                    : (amount - old->height) / Flt32(dim);
                            for (auto j :
                                 std::views::iota(Int32{0}) | std::views::take_while([&](Int32 j) {
                                     return j < dim;
                                 })) {
                                Point2I q{};
                                if (side == 2) {
                                    q = {p.x + j, dest.y};
                                } else if (side == 4) {
                                    q = {dest.x, p.y + j};
                                } else if (side == 8) {
                                    q = {p.x + dim - j - 1, dest.y};
                                } else {
                                    q = {dest.x, p.y + dim - j - 1};
                                }
                                const auto current = info(d, q);
                                if (current && !height(d, q, current->height + step * Flt32(j))) {
                                    return false;
                                }
                            }
                        }
                    } else {
                        const bool upper = n != -1, leftSide = m != -1;
                        const Point2I src{leftSide ? brush_.x + dim - 1 : brush_.x,
                                          leftSide ? brush_.y + dim - 1 : brush_.y};
                        const auto h = info(d, src);
                        if (h && !adjustControl(p, h->height, upper, leftSide)) {
                            return false;
                        }
                    }
                }
            }
            return true;
        }
        // Native unsnapped/detail-zero feather averages the touched one-cell ring
        // from the completed inner edit before writing any ring heights.
        std::vector<Point2I> touched;
        std::set<std::pair<Int32, Int32>> used;
        for (auto p : selected) {
            for (auto j : std::views::iota(Int32{-1}) | std::views::take_while([&](Int32 j) {
                              return j <= 1;
                          })) {
                for (auto i : std::views::iota(Int32{-1}) | std::views::take_while([&](Int32 i) {
                                  return i <= 1;
                              })) {
                    Point2I q{p.x + i, p.y + j};
                    if ((i || j) && pointValid(d, q) && !contains(selected, q) &&
                        used.emplace(q.x, q.y).second) {
                        touched.push_back(q);
                    }
                }
            }
        }
        std::vector<float> values;
        for (auto p : touched) {
            float sum{};
            UInt32 count{};
            for (auto j : std::views::iota(Int32{-1}) | std::views::take_while([&](Int32 j) {
                              return j <= 1;
                          })) {
                for (auto i : std::views::iota(Int32{-1}) | std::views::take_while([&](Int32 i) {
                                  return i <= 1;
                              })) {
                    const auto q = info(d, {p.x + i, p.y + j});
                    if (q) {
                        sum += q->height;
                        ++count;
                    }
                }
            }
            values.push_back(sum / Flt32(count));
        }
        for (auto i : std::views::iota(std::size_t{0}, touched.size())) {
            if (!height(d, touched[i], values[i])) {
                return false;
            }
        }
    }
    return true;
}

bool TerrainEditor::action(std::string_view name) {
    if (working_ || !snapshot()) {
        return false;
    }
    const auto a = lower(name);
    if (std::ranges::none_of(actionNames, [&](auto n) {
            return lower(n) == a;
        })) {
        return false;
    }
    if (a == "undo") {
        return undo();
    }
    if (a == "redo") {
        return redo();
    }
    if (a == "copy") {
        std::vector<TerrainSelectionInfo> copy;
        if (selection_.empty()) {
            clipboard_.clear();
            return true;
        }
        const auto start = selection_[0];
        for (auto p : selection_) {
            auto i = info(*snapshot(), p);
            if (i) {
                i->point.x -= start.x;
                i->point.y -= start.y;
                copy.push_back(*i);
            }
        }
        clipboard_ = std::move(copy);
        return true;
    }
    if (a == "getavgheight") {
        if (selection_.empty()) {
            return false;
        }
        float sum{};
        UInt32 count{};
        for (auto p : selection_) {
            auto i = info(*snapshot(), p);
            if (i) {
                sum += i->height;
                ++count;
            }
        }
        if (!count) {
            return false;
        }
        average_ = sum / Flt32(count);
        return true;
    }
    if (a == "select") {
        return brush(brush_);
    }
    if (a == "deselect") {
        selection_.clear();
        return true;
    }
    if (selection_.empty() || a == "noise" || a.starts_with("adjust") ||
        (a == "smooth" && !settings_.brushDetail)) {
        return true;
    }
    const auto ok = history_.edit([&](auto& d) {
        return mutate(d, a, selection_);
    });
    if (ok && a == "paste") {
        floating_ = false;
    }
    return ok;
}

bool TerrainEditor::gesture(std::string_view name,
                            TerrainGesture phase,
                            Point2I position,
                            Int32 mouseDelta) {
    if (phase == TerrainGesture::Cancel) {
        if (working_) {
            selection_ = savedSelection_;
        }
        working_.reset();
        gestureAction_.clear();
        gestureSelection_.clear();
        savedSelection_.clear();
        return true;
    }
    if (std::abs(Int64(mouseDelta)) > 100000 || !snapshot()) {
        return false;
    }
    const auto a = lower(name);
    if (phase == TerrainGesture::Begin) {
        if (working_ || std::ranges::none_of(actionNames, [&](auto n) {
                return lower(n) == a;
            })) {
            return false;
        }
        const auto persistent = selection_;
        if (!brush(position)) {
            return false;
        }
        gestureSelection_ =
            persistent.empty() || a == "select" || a == "deselect" ? selection_ : persistent;
        selection_ = persistent;
        savedSelection_ = persistent;
        working_ = std::make_shared<TerrainDocument>(*snapshot());
        gestureAction_ = a;
        grab_ = brush_;
        if (a.starts_with("adjust")) {
            return true;
        }
    } else if (!working_ || a != gestureAction_) {
        return false;
    }
    if (phase != TerrainGesture::Begin && !a.starts_with("adjust")) {
        const auto persistent = selection_;
        if (!brush(position)) {
            return false;
        }
        gestureSelection_ = savedSelection_.empty() || a == "select" || a == "deselect"
                                ? selection_
                                : savedSelection_;
        selection_ = persistent;
    }
    if (a == "select" || a == "deselect") {
        if (a == "select") {
            if (!select(gestureSelection_, true)) {
                return false;
            }
        } else {
            std::erase_if(selection_, [&](auto p) {
                return contains(gestureSelection_, p);
            });
        }
        if (phase == TerrainGesture::End) {
            working_.reset();
            gestureAction_.clear();
            savedSelection_.clear();
        }
        return true;
    }
    const bool oneShot = a == "copy" || a == "getavgheight" || a == "undo" || a == "redo";
    if (oneShot) {
        if (phase != TerrainGesture::End) {
            return true;
        }
        working_.reset();
        gestureAction_.clear();
        savedSelection_.clear();
        return action(a);
    }
    if (phase == TerrainGesture::End) {
        auto next = working_;
        if ((a == "pin" || a == "clearpin" || a == "relight") &&
            !mutate(*next, a, gestureSelection_)) {
            return false;
        }
        if (a != "noise" && !(a == "smooth" && !settings_.brushDetail) &&
            !history_.edit([&](auto& d) {
                d = *next;
                return true;
            })) {
            return false;
        }
        working_.reset();
        gestureAction_.clear();
        savedSelection_.clear();
        return true;
    }
    if (a == "pin" || a == "clearpin" || a == "relight" || a == "noise" ||
        (a == "smooth" && !settings_.brushDetail)) {
        return true;
    }
    auto next = std::make_shared<TerrainDocument>(*working_);
    if (!mutate(*next, a, gestureSelection_, mouseDelta) || !next->rebuildHeightRanges() ||
        !next->valid()) {
        return false;
    }
    working_ = std::move(next);
    return true;
}

bool TerrainEditor::mirror(UInt32 sides) {
    if (working_ || !snapshot() || snapshot()->grid.pattern != 0 || (sides & ~15U) ||
        ((sides & 1) && (sides & 4)) || ((sides & 2) && (sides & 8))) {
        return false;
    }
    return history_.edit([&](auto& d) {
        const auto n = Int32(d.blocks[0].width);
        Point2I src{(sides & 2) ? n - 1 : 0, (sides & 4) ? n - 1 : 0},
            dest{(sides & 8) ? n - 1 : 0, (sides & 1) ? n - 1 : 0};
        const auto os = src, od = dest;
        const bool diagonal = (sides & 10) && (sides & 5);
        auto minor = (sides & 5) ? n : n / 2;
        const auto major = (sides & 10) ? n : n / 2;
        const Point2I ss{(sides & 2) ? -1 : 1, (sides & 4) ? -1 : 1},
            ds{(sides & 8) ? -1 : 1, (sides & 1) ? -1 : 1};
        for ([[maybe_unused]] auto i :
             std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                 return i < major;
             })) {
            for ([[maybe_unused]] auto j :
                 std::views::iota(Int32{0}) | std::views::take_while([&](Int32 j) {
                     return j < minor;
                 })) {
                const auto h = info(d, src);
                if (!h || !height(d, dest, h->height)) {
                    return false;
                }
                src.x += ss.x;
                if (diagonal) {
                    dest.y += ds.y;
                } else {
                    dest.x += ds.x;
                }
            }
            src.y += ss.y;
            if (diagonal) {
                dest.x += ds.x;
            } else {
                dest.y += ds.y;
            }
            src.x = os.x;
            if (diagonal) {
                dest.y = od.y;
                --minor;
            } else {
                dest.x = od.x;
            }
        }
        return true;
    });
}

bool TerrainEditor::landscapeScript(std::string_view script, const Landscape::Loader& loader) {
    return !working_ && landscape.script(script, true, loader);
}

bool TerrainEditor::applyLandscape() {
    if (working_ || !snapshot()) {
        return false;
    }
    auto flipped = landscape;
    if (!flipped.execute({LandscapeOpcode::FlipY, {}, {}})) {
        return false;
    }
    const auto matrices = flipped.stack();
    if (matrices.empty()) {
        return false;
    }
    return history_.edit([&](auto& d) {
        const auto width = 1U << (d.grid.detailCount - 1);
        const auto apply = [&](Int32 id, Point2I origin) {
            const auto at = std::ranges::find(d.grid.blocks, id, &TerrainGridBlockEntry::id);
            return at != d.grid.blocks.end() &&
                   flipped.apply(d, UInt32(at - d.grid.blocks.begin()), origin);
        };
        if (d.grid.pattern == 0) {
            return apply(d.grid.blockMap[0], {});
        }
        if (d.grid.pattern == 1) {
            if (matrices[0].size != width * UInt32(d.grid.size.x)) {
                return false;
            }
            for (auto y : std::views::iota(Int32{0}, std::max(Int32{0}, d.grid.size.y))) {
                for (auto x : std::views::iota(Int32{0}, std::max(Int32{0}, d.grid.size.x))) {
                    if (!apply(d.grid.blockMap[std::size_t(y) * d.grid.size.x + x],
                               {x * Int32(width), y * Int32(width)})) {
                        return false;
                    }
                }
            }
            return true;
        }
        if (matrices[0].size != width * 4) {
            return false;
        }
        for (auto i : std::views::iota(Int32{0}, Int32{16})) {
            const Point2I origin{(i & 3) * Int32(width), (i >> 2) * Int32(width)};
            if (!apply(i, origin) || !apply(16 + i, origin)) {
                return false;
            }
        }
        return true;
    });
}

bool TerrainEditor::applyTextures() {
    return !working_ && history_.edit([&](auto& d) {
        return mapper.apply(d);
    });
}

namespace {
constexpr std::array commands{"initTed",
                              "quitTed",
                              "close",
                              "setSnap",
                              "setFeather",
                              "setBrushPos",
                              "setBrushDetail",
                              "setPinDetailVal",
                              "setPinDetailMax",
                              "setHeightVal",
                              "setAdjustVal",
                              "setScaleVal",
                              "setSmoothVal",
                              "setMatIndexVal",
                              "setFlagVal",
                              "setPasteVal",
                              "processAction",
                              "terrainAction",
                              "undo",
                              "redo",
                              "clearSelect",
                              "clearPinMaps",
                              "copy",
                              "paste",
                              "addNamedSelection",
                              "removeNamedSelection",
                              "selectNamedSelection",
                              "getBrushDetail",
                              "getMaxBrushDetail",
                              "getActionName",
                              "getNumActions",
                              "setTerrainType",
                              "getTerrainType",
                              "getMaterialIndex",
                              "save",
                              "open",
                              "new",
                              "saveSelection",
                              "loadSelection",
                              "mirrorGridBlock",
                              "LS::Script",
                              "LS::ApplyLandscape",
                              "LS::ApplyTextures",
                              "LS::Rules",
                              "LS::Textures",
                              "LS::Create",
                              "LS::flushTextures",
                              "LS::addTerrainType",
                              "LS::addTerrainTexture",
                              "LS::createGridFile",
                              "LS::flushRules",
                              "LS::addRule",
                              "LS::flushCommands",
                              "LS::addCommand",
                              "LS::parseCommands",
                              "attachToTerrain",
                              "setLButtonAction",
                              "setRButtonAction",
                              "getLButtonActionIndex",
                              "getRButtonActionIndex",
                              "relight",
                              "setFlags",
                              "clearFlags",
                              "toggleFlags",
                              "rotate",
                              "setUndoLimit",
                              "listCommands",
                              "listNamedSelections",
                              "selectMaterial",
                              "getNumTerrainTypes",
                              "getTerrainTypeName",
                              "assignMatList",
                              "loadMatList",
                              "getMaterialCount",
                              "getMaterialName",
                              "loadPalette",
                              "isActionMouseable",
                              "floatSelection",
                              "floatPaste",
                              "forceTerrainType"};
}

std::optional<std::string> TerrainEditor::command(std::string_view name,
                                                  std::span<const std::string> argv) {
    const auto a = lower(name);
    const auto yes = [](bool b) {
        return std::optional<std::string>(b ? "True" : "False");
    };
    if (a == "setsnap" || a == "setfeather") {
        const auto n = argv.size() == 1 ? boolean(argv[0]) : std::nullopt;
        if (!n) {
            return yes(false);
        }
        auto s = settings_;
        (a == "setsnap" ? s.snap : s.feather) = *n != 0;
        return yes(settings(s));
    }
    if (a == "attachtoterrain") {
        if (argv.size() != 1 || !terrainLoader || working_) {
            return yes(false);
        }
        auto d = terrainLoader(argv[0]);
        return yes(d && attach(std::move(*d)));
    }
    if (a == "floatselection" || a == "floatpaste") {
        return yes(argv.empty() && (a == "floatselection" ? floatSelection() : floatPaste()));
    }
    if (a == "setlbuttonaction" || a == "setrbuttonaction") {
        if (argv.size() != 1) {
            return yes(false);
        }
        const auto at = std::ranges::find_if(actionNames, [&](auto n) {
            return lower(n) == lower(argv[0]);
        });
        if (at == actionNames.end()) {
            return yes(false);
        }
        auto s = settings_;
        (a == "setlbuttonaction" ? s.leftAction : s.rightAction) = UInt32(at - actionNames.begin());
        return yes(settings(s));
    }
    if (a == "getlbuttonactionindex" || a == "getrbuttonactionindex") {
        return argv.empty() ? std::to_string(a == "getlbuttonactionindex" ? settings_.leftAction
                                                                          : settings_.rightAction)
                            : "False";
    }
    if (a == "setundolimit") {
        const auto n = argv.size() == 1 ? number(argv[0]) : std::nullopt;
        return yes(n && *n >= 0 && *n <= 1024 && *n == std::floor(*n) && !working_ &&
                   history_.limit(UInt32(*n)));
    }
    if (a == "listcommands" || a == "listnamedselections") {
        if (!argv.empty()) {
            return yes(false);
        }
        std::string result;
        if (a == "listcommands") {
            for (auto s : commands) {
                result += std::string(s) + "\n";
            }
        } else {
            for (const auto& [s, selection] : named_) {
                (void)selection;
                result += s + "\n";
            }
        }
        return result;
    }
    if (a == "relight") {
        if (argv.size() != 1 || !relight || working_) {
            return yes(false);
        }
        const auto hires = lower(argv[0]);
        if (hires != "true" && hires != "false" && hires != "0" && hires != "1") {
            return yes(false);
        }
        return yes(history_.edit([&](auto& d) {
            return relight(d, hires == "true" || hires == "1");
        }));
    }
    if (a == "setflags" || a == "clearflags" || a == "toggleflags" || a == "setflagval") {
        const auto mask = flags(argv);
        if (!mask) {
            return yes(false);
        }
        if (a == "setflagval") {
            auto s = settings_;
            s.flags = *mask;
            return yes(settings(s));
        }
        return yes(!working_ && history_.edit([&](auto& d) {
            for (auto p : selection_) {
                const auto old = info(d, p);
                if (!old) {
                    continue;
                }
                auto m = old->material;
                m.flags = a == "setflags"     ? UInt8(m.flags | *mask)
                          : a == "clearflags" ? UInt8(m.flags & ~*mask)
                                              : UInt8(m.flags ^ *mask);
                if (!material(d, p, m)) {
                    return false;
                }
            }
            return true;
        }));
    }
    if (a == "rotate") {
        return yes(argv.empty() && action("rotateLeft"));
    }
    if (a == "setpasteval" || (a == "paste" && !argv.empty())) {
        if (argv.size() != 1) {
            return yes(false);
        }
        const auto p = lower(argv[0]);
        auto s = settings_;
        const auto n = number(p);
        if (n && *n >= 0 && *n <= 3 && *n == std::floor(*n)) {
            s.paste = UInt8(*n);
        } else if (p == "all") {
            s.paste = 3;
        } else if (p == "height") {
            s.paste = 2;
        } else if (p == "material") {
            s.paste = 1;
        } else {
            return yes(false);
        }
        return yes(settings(s) && (a == "setpasteval" || action("paste")));
    }
    if (a == "forceterraintype") {
        const auto type = argv.size() == 1 ? number(argv[0]) : std::nullopt;
        if (!type || *type < 0 || *type > 255 || *type != std::floor(*type) || working_ ||
            !snapshot() || selection_.empty()) {
            return yes(false);
        }
        auto restore = checkpoint();
        try {
            auto s = settings_;
            s.terrainType = UInt8(*type);
            if (settings(s) && history_.edit([&](auto& d) {
                    // Original force delivery sets Edit/Corner around mapper apply.
                    // Publish those markers and the type as one undoable operation.
                    for (auto p : selection_) {
                        auto old = info(d, p);
                        if (!old) {
                            return false;
                        }
                        auto m = old->material;
                        m.flags |= 0xc0;
                        if (!material(d, p, m)) {
                            return false;
                        }
                    }
                    if (!mutate(d, "setTerrainType", selection_)) {
                        return false;
                    }
                    for (auto p : selection_) {
                        auto old = info(d, p);
                        if (!old) {
                            return false;
                        }
                        auto m = old->material;
                        m.flags |= 0xc0;
                        if (!material(d, p, m)) {
                            return false;
                        }
                    }
                    return true;
                })) {
                return yes(true);
            }
        } catch (...) {
            restore();
            return yes(false);
        }
        restore();
        return yes(false);
    }
    if (a == "selectmaterial") {
        if (argv.size() != 1) {
            return yes(false);
        }
        return command("setMatIndexVal", argv);
    }
    if (a == "getnumterraintypes") {
        return argv.empty() ? std::to_string(mapper.textures().types ? mapper.textures().types
                                                                     : textureRecipe.types.size())
                            : "False";
    }
    if (a == "getterraintypename" || a == "getmaterialname" || a == "isactionmouseable") {
        const auto n = argv.size() == 1 ? number(argv[0]) : std::nullopt;
        if (!n || *n < 0 || *n != std::floor(*n)) {
            return yes(false);
        }
        const auto i = std::size_t(*n);
        if (a == "isactionmouseable") {
            return yes(i < actionNames.size() && i != 1 && i != 5 && i != 9 && i != 11 && i != 12);
        }
        if (a == "getmaterialname") {
            const auto at = snapshot() ? materialLists_.find(snapshot()->grid.materialList)
                                       : materialLists_.end();
            return at != materialLists_.end() && i < at->second.materials.size()
                       ? at->second.materials[i].mapFile
                       : "False";
        }
        return mapper.textures().types
                   ? (i < mapper.textures().descriptions.size() ? mapper.textures().descriptions[i]
                                                                : "False")
                   : (i < textureRecipe.types.size() ? textureRecipe.types[i].second : "False");
    }
    if (a == "getmaterialcount") {
        const auto at =
            snapshot() ? materialLists_.find(snapshot()->grid.materialList) : materialLists_.end();
        return argv.empty()
                   ? std::to_string(at == materialLists_.end() ? 0 : at->second.materialsPerDetail)
                   : "False";
    }
    if (a == "assignmatlist" || a == "loadmatlist" || a == "loadpalette") {
        const auto bytes = argv.size() == 1 ? readFile(argv[0], MaxTsFileBytes) : std::nullopt;
        if (!bytes || working_) {
            return yes(false);
        }
        MemRWStream stream{std::span<const std::byte>(*bytes)};
        if (a == "loadpalette") {
            auto p = gfx::Palette::load(stream, UInt32(bytes->size()));
            if (!p) {
                return yes(false);
            }
            palette = std::move(*p);
            return yes(true);
        }
        auto list = TsMaterialList::load(stream, UInt32(bytes->size()));
        if (!list || (materialLists_.size() >= 64 && !materialLists_.contains(argv[0])) ||
            !history_.edit([&](auto& d) {
                d.grid.materialList = argv[0];
                return true;
            })) {
            return yes(false);
        }
        materialLists_[argv[0]] = std::move(*list);
        return yes(true);
    }
    if (a == "initted") {
        return yes(true);
    }
    if (a == "quitted" || a == "close") {
        if (!argv.empty()) {
            return yes(false);
        }
        detach();
        return yes(true);
    }
    if (a == "undo" || a == "redo" || a == "copy" || a == "paste") {
        return yes(argv.empty() && action(a));
    }
    if (a == "processaction") {
        if (argv.size() != 1) {
            return yes(false);
        }
        const auto detail = settings_.brushDetail;
        settings_.brushDetail = 0;
        const auto ok = action(argv[0]);
        settings_.brushDetail = detail;
        return yes(ok);
    }
    if (a == "clearselect") {
        if (!argv.empty()) {
            return yes(false);
        }
        selection_.clear();
        return yes(true);
    }
    if (a == "clearpinmaps") {
        return yes(argv.empty() && !working_ && history_.edit([](auto& d) {
            for (auto& b : d.blocks) {
                for (auto& p : b.pins) {
                    p.clear();
                }
            }
            return true;
        }));
    }
    if (a == "addnamedselection" || a == "removenamedselection" || a == "selectnamedselection") {
        return yes(argv.size() == 1 && (a == "addnamedselection"      ? nameSelection(argv[0])
                                        : a == "removenamedselection" ? removeSelection(argv[0])
                                                                      : chooseSelection(argv[0])));
    }
    if (a == "getbrushdetail") {
        return argv.empty() ? std::to_string(settings_.brushDetail) : "False";
    }
    if (a == "getmaxbrushdetail") {
        return snapshot() && argv.empty() ? std::to_string(snapshot()->grid.detailCount) : "False";
    }
    if (a == "getnumactions") {
        return argv.empty() ? std::to_string(actionNames.size()) : "False";
    }
    if (a == "getterrainType" || a == "getterraintype") {
        return std::to_string(settings_.terrainType);
    }
    if (a == "getmaterialindex") {
        return std::to_string(settings_.material);
    }
    if (a == "ls::applylandscape") {
        return yes(argv.empty() && applyLandscape());
    }
    if (a == "ls::applytextures") {
        return yes(argv.empty() && applyTextures());
    }
    if (a == "ls::create") {
        return yes(argv.empty() && bool(snapshot()));
    }
    if (a == "ls::flushtextures") {
        if (!argv.empty()) {
            return yes(false);
        }
        textureRecipe = {};
        return yes(true);
    }
    if (a == "ls::addterraintype") {
        return yes(argv.size() == 2 && argv[0].size() == 1 &&
                   textureRecipe.addType(argv[0][0], argv[1]));
    }
    if (a == "ls::addterraintexture") {
        if (argv.size() != 7) {
            return yes(false);
        }
        const auto rotation = number(argv[2]), percent = number(argv[3]),
                   elasticity = number(argv[5]), friction = number(argv[6]);
        return yes(rotation && percent && elasticity && friction && *rotation >= 1 &&
                   *rotation <= 255 && *rotation == std::floor(*rotation) && *percent >= 0 &&
                   *percent <= 255 && *percent == std::floor(*percent) &&
                   textureRecipe.addTexture(argv[0],
                                            argv[1],
                                            UInt8(*rotation),
                                            UInt8(*percent),
                                            argv[4],
                                            Flt32(*elasticity),
                                            Flt32(*friction)));
    }
    if (a == "ls::creategridfile") {
        if (argv.size() != 2) {
            return yes(false);
        }
        const auto table = textureRecipe.build();
        const auto dml = textureRecipe.materialList();
        const auto grid = table ? table->encode() : std::nullopt;
        return yes(grid && dml &&
                   writeFiles(std::array<OutputFile, 2>{{{argv[0], *grid}, {argv[1], *dml}}}));
    }
    if (a == "ls::flushrules") {
        if (!argv.empty()) {
            return yes(false);
        }
        mapper.clearRules();
        return yes(true);
    }
    if (a == "ls::addrule") {
        if (argv.size() != 13 || argv[0].size() != 1) {
            return yes(false);
        }
        auto tag = argv[0][0];
        if (tag >= 'a' && tag <= 'z') {
            tag = char(tag - 32);
        }
        const auto type =
            std::ranges::find(textureRecipe.types, tag, &std::pair<char, std::string>::first);
        if (type == textureRecipe.types.end()) {
            return yes(false);
        }
        std::array<double, 12> v{};
        for (auto j : std::views::iota(UInt32{0}, UInt32{12})) {
            const auto n = j == 5 || j == 11 ? boolean(argv[j + 1]) : number(argv[j + 1]);
            if (!n) {
                return yes(false);
            }
            v[j] = *n;
        }
        LandscapeRule r;
        r.group = Int32(type - textureRecipe.types.begin());
        r.altitude = {Flt32(v[0]), Flt32(v[1]), Flt32(v[2]), Flt32(v[3])};
        r.altitudeWeight = Flt32(v[4]);
        r.adjustHeights = v[5] != 0;
        r.slope = {Flt32(v[6]), Flt32(v[7]), Flt32(v[8]), Flt32(v[9])};
        r.slopeWeight = Flt32(v[10]);
        r.adjustSlopes = v[11] != 0;
        auto rules = mapper.rules();
        rules.push_back(r);
        return yes(mapper.setRules(std::move(rules)));
    }
    if (a == "ls::flushcommands") {
        if (!argv.empty()) {
            return yes(false);
        }
        landscapeCommands_.clear();
        return yes(true);
    }
    if (a == "ls::addcommand") {
        if (argv.size() != 1 || argv[0].size() > 254 ||
            landscapeCommands_.size() + argv[0].size() > 1024U * 1024U) {
            return yes(false);
        }
        landscapeCommands_ += argv[0] + "\n";
        return yes(true);
    }
    if (a == "ls::script" || a == "ls::parsecommands") {
        const bool file = a == "ls::script";
        if (argv.size() != (file ? 1U : 0U) || working_) {
            return yes(false);
        }
        const auto bytes =
            file ? readFile(argv[0], 1024U * 1024U + 5) : std::optional<std::vector<std::byte>>{};
        const auto record = bytes ? readLandscapeScript(*bytes, 1024U * 1024U) : std::nullopt;
        if (file && !record) {
            return yes(false);
        }
        const auto text = file ? record->text : landscapeCommands_;
        const auto root =
            file ? std::filesystem::path(argv[0]).parent_path() : std::filesystem::path{};
        auto candidate = landscape;
        const auto loader = [&](std::string_view name) -> std::optional<Bitmap> {
            FileRStream input(root / std::filesystem::path(name));
            if (input.getStatus() != STRM_OK) {
                return {};
            }
            auto bitmap = Bitmap::load(input, input.getSize());
            if (!bitmap) {
                return {};
            }
            return std::move(*bitmap);
        };
        if (!candidate.script(text, true, loader)) {
            return yes(false);
        }
        std::vector<OutputFile> outputs;
        for (const auto& output : candidate.outputs()) {
            std::vector<std::byte> data(output.bitmap.pixels.size() + 4096);
            MemRWStream stream{std::span<std::byte>(data)};
            if (!output.bitmap.write(stream)) {
                return yes(false);
            }
            data.resize(std::size_t(stream.getPosition()));
            outputs.emplace_back(root / output.name, std::move(data));
        }
        if (!writeFiles(outputs)) {
            return yes(false);
        }
        landscape = std::move(candidate);
        return yes(true);
    }
    if (a == "ls::rules") {
        const auto bytes = argv.size() == 1 ? readFile(argv[0], 1024U * 1024U) : std::nullopt;
        const auto rules = bytes ? readLandscapeRules(std::string_view(
                                       reinterpret_cast<const char*>(bytes->data()), bytes->size()))
                                 : std::nullopt;
        return yes(rules && mapper.setRules(*rules));
    }
    if (a == "ls::textures") {
        if (argv.size() != 2 || working_ || !snapshot()) {
            return yes(false);
        }
        const auto bytes = readFile(argv[1], 32U * 1024U * 1024U),
                   dml = readFile(argv[0], MaxTsFileBytes);
        const auto table = bytes ? LandscapeTextureTable::load(*bytes) : std::nullopt;
        if (!table || !dml) {
            return yes(false);
        }
        MemRWStream stream{std::span<const std::byte>(*dml)};
        const auto list = TsMaterialList::load(stream, UInt32(dml->size()));
        if (!list || list->materialsPerDetail != Int32(table->corners.size())) {
            return yes(false);
        }
        auto next = mapper;
        if ((materialLists_.size() >= 64 && !materialLists_.contains(argv[0])) ||
            !next.setTextures(*table) || !history_.edit([&](auto& d) {
                d.grid.materialList = argv[0];
                return true;
            })) {
            return yes(false);
        }
        mapper = std::move(next);
        materialLists_[argv[0]] = *list;
        return yes(true);
    }
    if (a == "save") {
        return yes(argv.size() == 1 && save(std::filesystem::path(argv[0])));
    }
    if (a == "open" || a == "loadselection") {
        if (argv.size() != 1) {
            return yes(false);
        }
        FileRStream f{std::filesystem::path(argv[0])};
        if (f.getStatus() != STRM_OK || f.getSize() > 256U * 1024U * 1024U) {
            return yes(false);
        }
        std::vector<std::byte> bytes(f.getSize());
        if (!f.readBytes(bytes)) {
            return yes(false);
        }
        return yes(a == "open" ? open(bytes) : loadSelection(bytes));
    }
    if (a == "saveselection") {
        if (argv.size() != 1) {
            return yes(false);
        }
        auto bytes = saveSelection();
        if (!bytes) {
            return yes(false);
        }
        FileWStream f{std::filesystem::path(argv[0])};
        return yes(f.getStatus() == STRM_OK && f.writeBytes(*bytes) && f.flush());
    }
    if (a == "new") {
        if (argv.size() != 5) {
            return yes(false);
        }
        std::array<Int32, 5> v{};
        for (auto j : std::views::iota(std::size_t{0}, std::size_t{5})) {
            auto n = number(argv[j]);
            if (!n || *n != std::floor(*n)) {
                return yes(false);
            }
            v[j] = Int32(*n);
        }
        auto d = TerrainDocument::create({v[0], v[1]}, UInt32(v[2]), v[3], v[4], 0);
        return yes(d && attach(std::move(*d)));
    }
    if (a == "terrainaction") {
        if (argv.size() != 1 || !snapshot() || snapshot()->grid.pattern != 0) {
            return yes(false);
        }
        const auto old = selection_;
        const auto w = Int32(snapshot()->blocks[0].width);
        if (!region({0, 0}, {w - 1, w - 1})) {
            return yes(false);
        }
        const auto ok = action(argv[0]);
        selection_ = old;
        return yes(ok);
    }
    if (a == "setbrushpos") {
        if (argv.size() != 2) {
            return yes(false);
        }
        auto x = number(argv[0]), y = number(argv[1]);
        return yes(x && y && *x == std::floor(*x) && *y == std::floor(*y) &&
                   brush({Int32(*x), Int32(*y)}));
    }
    if (a == "mirrorgridblock" && argv.size() == 1 && !number(argv[0])) {
        const auto s = lower(argv[0]);
        UInt32 sides{};
        if (s.find("top") != s.npos) {
            sides |= 1;
        }
        if (s.find("right") != s.npos) {
            sides |= 2;
        }
        if (s.find("bottom") != s.npos) {
            sides |= 4;
        }
        if (s.find("left") != s.npos) {
            sides |= 8;
        }
        return yes(sides && mirror(sides));
    }
    if (argv.size() != 1) {
        return yes(false);
    }
    const auto n = number(argv[0]);
    if (!n) {
        return yes(false);
    }
    auto s = settings_;
    const auto integer = *n == std::floor(*n) && *n >= 0;
    if (a == "getactionname") {
        return integer && *n < actionNames.size()
                   ? std::optional<std::string>(actionNames[std::size_t(*n)])
                   : yes(false);
    }
    if (a == "mirrorgridblock") {
        return yes(integer && mirror(UInt32(*n)));
    }
    if (a == "setsnap") {
        s.snap = *n != 0;
    } else if (a == "setfeather") {
        s.feather = *n != 0;
    } else if (a == "setheightval") {
        s.height = Flt32(*n);
    } else if (a == "setadjustval") {
        s.adjust = Flt32(*n);
    } else if (a == "setscaleval") {
        s.scale = Flt32(*n);
    } else if (a == "setsmoothval") {
        s.smooth = std::clamp(Flt32(*n), 0.f, 1.f);
    } else if (!integer) {
        return yes(false);
    } else if (a == "setbrushdetail") {
        s.brushDetail = UInt32(*n);
    } else if (a == "setpindetailval") {
        s.pinDetail = UInt32(*n);
    } else if (a == "setpindetailmax") {
        s.pinMax = UInt32(*n);
    } else if (*n > 255) {
        return yes(false);
    } else if (a == "setmatindexval") {
        s.material = UInt8(*n);
    } else if (a == "setflagval") {
        s.flags = UInt8(*n);
    } else if (a == "setpasteval") {
        s.paste = UInt8(*n);
    } else if (a == "setterraintype") {
        s.terrainType = UInt8(*n);
    } else {
        return yes(false);
    }
    return yes(settings(s));
}

void installTerrainEditorCommands(Console& console, const std::shared_ptr<TerrainEditor>& editor) {
    std::weak_ptr<TerrainEditor> weak = editor;
    Int32 id{};
    for (std::string_view command : commands) {
        const auto name =
            command.starts_with("LS::") ? std::string(command) : "Ted::" + std::string(command);
        console.addCommand(id++,
                           name,
                           [weak, command](Console&, Int32, std::span<const std::string> argv)
                               -> std::optional<std::string> {
                               const auto editor = weak.lock();
                               return editor ? editor->command(command, argv.subspan(1))
                                             : std::optional<std::string>("False");
                           });
    }
}

void uninstallTerrainEditorCommands(Console& console) {
    for (std::string_view command : commands) {
        console.removeCommand(command.starts_with("LS::") ? std::string(command)
                                                          : "Ted::" + std::string(command));
    }
}
} // namespace opent1::world
