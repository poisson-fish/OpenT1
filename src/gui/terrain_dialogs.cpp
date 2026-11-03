#include <opent1/gui/terrain_tools.hpp>
#include <ranges>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <charconv>
#include <cmath>

namespace opent1::gui {
namespace {
bool number(std::string_view text, double& value) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size() && std::isfinite(value) &&
           std::abs(value) <= 1.e7;
}

std::optional<std::vector<std::byte>> read(std::string_view name) {
    FileRStream input{std::filesystem::path(name)};
    if (input.getStatus() != STRM_OK || input.getSize() > 256U * 1024U * 1024U) {
        return {};
    }
    std::vector<std::byte> bytes(input.getSize());
    if (!input.readBytes(bytes)) {
        return {};
    }
    return bytes;
}

std::optional<std::vector<std::byte>> bitmapBytes(const gfx::Bitmap& image) {
    std::vector<std::byte> bytes(image.pixels.size() + 65536);
    MemRWStream output{std::span<std::byte>(bytes)};
    if (!image.write(output)) {
        return {};
    }
    bytes.resize(output.getPosition());
    return bytes;
}
}

bool TerrainTools::runScript(std::string_view text, const world::Landscape::Loader& supplied) {
    if (editor_->preview() != editor_->snapshot()) {
        return false;
    }
    try {
        auto next = editor_->landscape;
        const world::Landscape::Loader loader =
            supplied ? supplied
                     : world::Landscape::Loader(
                           [&](std::string_view name) -> std::optional<gfx::Bitmap> {
                               const auto path =
                                   (scriptPath_.parent_path() / std::filesystem::path(name))
                                       .generic_string();
                               auto bytes = readFile ? readFile(path) : read(path);
                               if (!bytes || bytes->size() > 64U * 1024U * 1024U) {
                                   return {};
                               }
                               MemRWStream input{std::span<const std::byte>(*bytes)};
                               auto bitmap = gfx::Bitmap::load(input, UInt32(bytes->size()));
                               return bitmap ? std::optional(std::move(*bitmap)) : std::nullopt;
                           });
        if (!next.script(text, true, loader)) {
            return false;
        }
        std::vector<world::TerrainOutputFile> outputs;
        for (const auto& output : next.outputs()) {
            auto bytes = bitmapBytes(output.bitmap);
            if (!bytes) {
                return false;
            }
            outputs.emplace_back(scriptPath_.parent_path() / output.name, std::move(*bytes));
        }
        if (!outputs.empty() &&
            !(writeFiles ? writeFiles(outputs) : world::writeTerrainEditorFiles(outputs))) {
            return false;
        }
        editor_->landscape = std::move(next);
        return true;
    } catch (...) {
        return false;
    }
}

bool TerrainTools::file(TerrainFileAction action, std::string_view path, std::string_view second) {
    if (path.empty() || path.size() > 4096 || path.find('\0') != path.npos ||
        second.size() > 4096 || second.find('\0') != second.npos ||
        editor_->preview() != editor_->snapshot()) {
        return false;
    }
    const auto load = [&](std::string_view name) {
        return readFile ? readFile(name) : read(name);
    };
    const auto save = [&](std::vector<world::TerrainOutputFile> outputs) {
        return writeFiles ? writeFiles(outputs) : world::writeTerrainEditorFiles(outputs);
    };
    try {
        if (action == TerrainFileAction::Save) {
            auto d = editor_->snapshot();
            auto bytes = d ? d->encode() : std::nullopt;
            return bytes && save({{std::filesystem::path(path), std::move(*bytes)}});
        }
        if (action == TerrainFileAction::SaveSelection) {
            auto bytes = editor_->saveSelection();
            return bytes && save({{std::filesystem::path(path), std::move(*bytes)}});
        }
        if (action == TerrainFileAction::SaveRules) {
            auto text = world::writeLandscapeRules(editor_->mapper.rules());
            if (!text) {
                return false;
            }
            auto bytes = std::as_bytes(std::span(*text));
            return save({{std::filesystem::path(path), {bytes.begin(), bytes.end()}}});
        }
        if (action == TerrainFileAction::SaveScript) {
            auto bytes = world::writeLandscapeScript(script.text, scriptFormat_);
            return bytes && save({{std::filesystem::path(path), std::move(*bytes)}});
        }
        if (action == TerrainFileAction::SaveBitmap) {
            auto image = preview.snapshot();
            auto bytes = image ? bitmapBytes(*image) : std::nullopt;
            return bytes && save({{std::filesystem::path(path), std::move(*bytes)}});
        }
        auto bytes = load(path);
        if (!bytes || bytes->size() > 256U * 1024U * 1024U) {
            return false;
        }
        MemRWStream input{std::span<const std::byte>(*bytes)};
        if (action == TerrainFileAction::Open) {
            return editor_->open(*bytes);
        }
        if (action == TerrainFileAction::LoadSelection) {
            return editor_->loadSelection(*bytes);
        }
        if (action == TerrainFileAction::LoadRules) {
            if (bytes->size() > 1024U * 1024U) {
                return false;
            }
            auto rules = world::readLandscapeRules(
                std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
            return rules && editor_->mapper.setRules(std::move(*rules));
        }
        if (action == TerrainFileAction::LoadScript) {
            auto record = world::readLandscapeScript(*bytes);
            if (!record) {
                return false;
            }
            auto stagedPath = std::filesystem::path(path);
            if (!script.setText(std::move(record->text))) {
                return false;
            }
            scriptPath_ = std::move(stagedPath);
            scriptFormat_ = record->format;
            return true;
        }
        if (action == TerrainFileAction::LoadBitmap) {
            auto image = gfx::Bitmap::load(input, UInt32(bytes->size()));
            if (!image) {
                return false;
            }
            auto next = editor_->landscape;
            if (!next.importBitmap(*image)) {
                return false;
            }
            editor_->landscape = std::move(next);
            return true;
        }
        if (action == TerrainFileAction::LoadPalette) {
            auto palette = gfx::Palette::load(input, UInt32(bytes->size()));
            if (!palette) {
                return false;
            }
            editor_->palette = std::move(*palette);
            return true;
        }
        if (action == TerrainFileAction::LoadMaterials ||
            action == TerrainFileAction::LoadTextures) {
            if (bytes->size() > world::MaxTsFileBytes) {
                return false;
            }
            auto list = world::TsMaterialList::load(input, UInt32(bytes->size()));
            if (!list) {
                return false;
            }
            ResourceManager owner;
            for (const auto& name : list->dependencies()) {
                auto data =
                    load((std::filesystem::path(path).parent_path() / name).generic_string());
                if (!data || data->size() > 64U * 1024U * 1024U) {
                    return false;
                }
                MemRWStream stream{std::span<const std::byte>(*data)};
                auto image = gfx::Bitmap::load(stream, UInt32(data->size()));
                if (!image || !owner.add(name, std::move(image))) {
                    return false;
                }
            }
            auto prepared = world::TsMaterialResources::load(owner, *list);
            if (!prepared) {
                return false;
            }
            std::optional<std::vector<std::byte>> textures;
            if (action == TerrainFileAction::LoadTextures) {
                textures = load(second);
                if (!textures || textures->size() > 32U * 1024U * 1024U) {
                    return false;
                }
            }
            if (!editor_->materialResources(
                    path,
                    *bytes,
                    textures ? std::optional(std::span<const std::byte>(*textures))
                             : std::nullopt)) {
                return false;
            }
            view.materialResources(std::move(*prepared));
            return true;
        }
    } catch (...) {
        return false;
    }
    return false;
}

bool TerrainTools::addDialogField(std::string label, std::string value) {
    const auto index = edits_.size();
    auto edit = std::make_unique<TextEdit>();
    auto text = std::make_unique<Text>();
    edit->text = std::move(value);
    edit->maxBytes = 4096;
    edit->font = font_;
    text->text = std::move(label);
    text->font = font_;
    const auto x = Int32(index % 2) * 280, y = Int32(index / 2) * 28;
    if (!text->resize({x, y}, {130, 24}) || !edit->resize({x + 132, y}, {140, 24}) ||
        !editorDialog_.addObject(*text) || !editorDialog_.addObject(*edit)) {
        return false;
    }
    labels_.push_back(std::move(text));
    edits_.push_back(std::move(edit));
    return true;
}

bool TerrainTools::showEditorDialog() {
    const auto contentHeight = Int32((edits_.size() + 1) / 2) * 28;
    const auto extent = tedRoot_.fields().extent;
    const auto width = std::min(576, extent.x - 16),
               height = std::min(std::max(100, contentHeight + 34), extent.y - 16);
    if (width < 208 || height < 100) {
        return false;
    }
    if (!editorDialog_.resize({}, {560, contentHeight}) ||
        !editorModal_.resize({8, 8}, {width, height}) ||
        !editorScroll_.resize({}, {width, height - 30})) {
        return false;
    }
    editorScroll_.horizontalPolicy = Scroll::Dynamic;
    editorScroll_.verticalPolicy = Scroll::Dynamic;
    if (!editorScroll_.parent() &&
        (!editorModal_.addObject(editorScroll_) || !editorScroll_.addObject(editorDialog_))) {
        return false;
    }
    for (auto i : std::views::iota(0, 2)) {
        auto b = std::make_unique<Button>();
        b->text = i ? "Cancel" : "OK";
        b->message = i ? 10 : 9;
        b->font = b->highlightFont = font_;
        if (!b->resize({i * 104, height - 28}, {100, 24}) || !editorModal_.addObject(*b)) {
            return false;
        }
        dialogButtons_.push_back(std::move(b));
    }
    if (auto* types = editorModal_.findTag(65557)) {
        types->visible(edits_.size() == 13);
        if (!types->resize({208, height - 28}, {72, 24})) {
            return false;
        }
    }
    return editorScroll_.computeSizes() && ted.pushDialog(editorModal_);
}

bool TerrainTools::dialog(TerrainDialog kind, std::size_t rule) {
    if (ted.dialogCount() || kind == TerrainDialog::File || kind == TerrainDialog::Value) {
        return false;
    }
    edits_.clear();
    labels_.clear();
    dialogButtons_.clear();
    dialog_.reset();
    if (kind == TerrainDialog::Settings) {
        const auto& s = editor_->settings();
        const std::array<std::string_view, 18> names{"Height",
                                                     "Adjust",
                                                     "Scale",
                                                     "Smooth",
                                                     "Mouse adjust",
                                                     "Brush detail",
                                                     "Pin detail",
                                                     "Pin maximum",
                                                     "Left action",
                                                     "Right action",
                                                     "Material",
                                                     "Flags",
                                                     "Paste",
                                                     "Terrain type",
                                                     "Snap (0/1)",
                                                     "Feather (0/1)",
                                                     "Selection color",
                                                     "Highlight color"};
        const std::array<double, 18> values{s.height,
                                            s.adjust,
                                            s.scale,
                                            s.smooth,
                                            s.mouseAdjust,
                                            double(s.brushDetail),
                                            double(s.pinDetail),
                                            double(s.pinMax),
                                            double(s.leftAction),
                                            double(s.rightAction),
                                            double(s.material),
                                            double(s.flags),
                                            double(s.paste),
                                            double(s.terrainType),
                                            double(s.snap),
                                            double(s.feather),
                                            double(view.display.selectionColor),
                                            double(view.display.highlightColor)};
        for (auto i : std::views::iota(std::size_t{0}, names.size())) {
            if (!addDialogField(std::string(names[i]), std::format("{}", values[i]))) {
                return false;
            }
        }
    } else if (kind == TerrainDialog::NewTerrain) {
        for (auto [name, value] :
             std::array<std::pair<std::string_view, std::string_view>, 5>{{{"Blocks X", "1"},
                                                                           {"Blocks Y", "1"},
                                                                           {"Block width", "256"},
                                                                           {"Scale", "3"},
                                                                           {"Pattern", "0"}}}) {
            if (!addDialogField(std::string(name), std::string(value))) {
                return false;
            }
        }
    } else if (kind == TerrainDialog::Rule) {
        const auto& rules = editor_->mapper.rules();
        if (rule > rules.size() || rules.size() >= 4096) {
            return false;
        }
        const auto r = rule < rules.size() ? rules[rule] : world::LandscapeRule{};
        const std::array<std::string_view, 13> names{"Terrain type",
                                                     "Altitude minimum",
                                                     "Altitude maximum",
                                                     "Altitude mean",
                                                     "Altitude deviation",
                                                     "Altitude weight",
                                                     "Adjust heights",
                                                     "Slope minimum",
                                                     "Slope maximum",
                                                     "Slope mean",
                                                     "Slope deviation",
                                                     "Slope weight",
                                                     "Adjust slopes"};
        const std::array<double, 13> values{double(r.group),
                                            r.altitude.minimum,
                                            r.altitude.maximum,
                                            r.altitude.mean,
                                            r.altitude.deviation,
                                            r.altitudeWeight,
                                            double(r.adjustHeights),
                                            r.slope.minimum,
                                            r.slope.maximum,
                                            r.slope.mean,
                                            r.slope.deviation,
                                            r.slopeWeight,
                                            double(r.adjustSlopes)};
        for (auto i : std::views::iota(std::size_t{0}, names.size())) {
            if (!addDialogField(std::string(names[i]), std::format("{}", values[i]))) {
                return false;
            }
        }
    } else {
        return false;
    }
    if (!showEditorDialog()) {
        return false;
    }
    dialog_ = kind;
    rule_ = rule;
    return true;
}

bool TerrainTools::valueDialog(UInt32 command) {
    if (ted.dialogCount() || !editor_->snapshot() ||
        ((command == 706 || command == 65558) && editor_->selection().empty())) {
        return false;
    }
    const auto& s = editor_->settings();
    std::string label;
    double value{};
    switch (command) {
        case 1038:
            label = "Adjust height";
            value = s.adjust;
            break;
        case 1039:
            label = "Set height";
            value = s.height;
            break;
        case 1040:
            label = "Material index";
            value = s.material;
            break;
        case 1041:
            label = "Flag mask";
            value = s.flags;
            break;
        case 1042:
            label = "Paste mask";
            value = s.paste;
            break;
        case 1050:
            label = "Scale value";
            value = s.scale;
            break;
        case 1048:
        case 706:
        case 65558:
            label = "Terrain type";
            value = s.terrainType;
            break;
        case 1051:
            label = "Pin detail level";
            value = s.pinDetail;
            break;
        case 1059:
            label = "Pin detail max";
            value = s.pinMax;
            break;
        case 1052:
            label = "Smooth value";
            value = s.smooth;
            break;
        default:
            return false;
    }
    auto selected = editor_->selection();
    const auto revision = editor_->revision();
    edits_.clear();
    labels_.clear();
    dialogButtons_.clear();
    dialog_.reset();
    if (!addDialogField(std::move(label), std::format("{}", value)) || !showEditorDialog()) {
        return false;
    }
    dialog_ = TerrainDialog::Value;
    valueCommand_ = command;
    valueRevision_ = revision;
    valueSelection_ = std::move(selected);
    return true;
}

bool TerrainTools::fileDialog(TerrainFileAction action) {
    if (ted.dialogCount()) {
        return false;
    }
    edits_.clear();
    labels_.clear();
    dialogButtons_.clear();
    dialog_.reset();
    if (!addDialogField("File", "") ||
        (action == TerrainFileAction::LoadTextures && !addDialogField("Texture table", "")) ||
        !showEditorDialog()) {
        return false;
    }
    dialog_ = TerrainDialog::File;
    fileAction_ = action;
    return true;
}

bool TerrainTools::dialogValue(std::size_t index, std::string value) {
    if (!dialog_ || index >= edits_.size() || value.size() > edits_[index]->maxBytes ||
        value.find('\0') != value.npos) {
        return false;
    }
    edits_[index]->text = std::move(value);
    edits_[index]->cursor = edits_[index]->anchor = 0;
    return true;
}

std::optional<std::string> TerrainTools::dialogValue(std::size_t index) const {
    return dialog_ && index < edits_.size() ? std::optional(edits_[index]->text) : std::nullopt;
}

bool TerrainTools::cancelEditorDialog() {
    if (!dialog_ || !ted.popDialog()) {
        return false;
    }
    dialog_.reset();
    return true;
}

bool TerrainTools::removeRule(std::size_t index) {
    auto rules = editor_->mapper.rules();
    if (index >= rules.size() || dialog_) {
        return false;
    }
    rules.erase(rules.begin() + std::ptrdiff_t(index));
    if (rules.empty()) {
        editor_->mapper.clearRules();
        return true;
    }
    return editor_->mapper.setRules(std::move(rules));
}

bool TerrainTools::acceptDialog() {
    if (!dialog_) {
        return false;
    }
    auto restore = view.checkpoint();
    auto restoreScript = script.checkpoint();
    const auto oldPath = scriptPath_;
    const auto oldFormat = scriptFormat_;
    bool accepted = false;
    std::vector<double> values;
    if (*dialog_ != TerrainDialog::File) {
        for (const auto& edit : edits_) {
            double n{};
            if (!number(edit->text, n)) {
                return false;
            }
            values.push_back(n);
        }
    }
    const auto integer = [&](std::size_t index, double low, double high) {
        return values[index] >= low && values[index] <= high &&
               values[index] == std::floor(values[index]);
    };
    const auto commit = [&]() -> bool {
        if (*dialog_ == TerrainDialog::Settings) {
            auto next = editor_->settings();
            for (auto i : std::views::iota(std::size_t{5}, std::size_t{18})) {
                if (!integer(i, 0, i == 14 || i == 15 ? 1 : 255)) {
                    return false;
                }
            }
            next.height = float(values[0]);
            next.adjust = float(values[1]);
            next.scale = float(values[2]);
            next.smooth = float(values[3]);
            next.mouseAdjust = float(values[4]);
            next.brushDetail = UInt32(values[5]);
            next.pinDetail = UInt32(values[6]);
            next.pinMax = UInt32(values[7]);
            next.leftAction = UInt32(values[8]);
            next.rightAction = UInt32(values[9]);
            next.material = UInt8(values[10]);
            next.flags = UInt8(values[11]);
            next.paste = UInt8(values[12]);
            next.terrainType = UInt8(values[13]);
            next.snap = values[14] != 0;
            next.feather = values[15] != 0;
            accepted = editor_->settings(next);
            if (accepted) {
                view.display.selectionColor = UInt8(values[16]);
                view.display.highlightColor = UInt8(values[17]);
            }
        } else if (*dialog_ == TerrainDialog::NewTerrain) {
            for (auto i : std::views::iota(std::size_t{0}, std::size_t{5})) {
                if (!integer(i, 0, 16384)) {
                    return false;
                }
            }
            auto document = world::TerrainDocument::create({Int32(values[0]), Int32(values[1])},
                                                           UInt32(values[2]),
                                                           Int32(values[3]),
                                                           Int32(values[4]),
                                                           0);
            accepted = document && editor_->attach(std::move(*document));
        } else if (*dialog_ == TerrainDialog::Rule) {
            if (!integer(0, 0, 255) || !integer(6, 0, 1) || !integer(12, 0, 1)) {
                return false;
            }
            world::LandscapeRule r;
            r.group = Int32(values[0]);
            r.altitude = {float(values[1]), float(values[2]), float(values[3]), float(values[4])};
            r.altitudeWeight = float(values[5]);
            r.adjustHeights = values[6] != 0;
            r.slope = {float(values[7]), float(values[8]), float(values[9]), float(values[10])};
            r.slopeWeight = float(values[11]);
            r.adjustSlopes = values[12] != 0;
            auto rules = editor_->mapper.rules();
            if (rule_ > rules.size()) {
                return false;
            }
            if (rule_ == rules.size()) {
                rules.push_back(r);
            } else {
                rules[rule_] = r;
            }
            accepted = editor_->mapper.setRules(std::move(rules));
        } else if (*dialog_ == TerrainDialog::Value) {
            std::string_view name;
            switch (valueCommand_) {
                case 1038:
                    name = "setAdjustVal";
                    break;
                case 1039:
                    name = "setHeightVal";
                    break;
                case 1040:
                    name = "setMatIndexVal";
                    break;
                case 1041:
                    name = "setFlagVal";
                    break;
                case 1042:
                    name = "setPasteVal";
                    break;
                case 1050:
                    name = "setScaleVal";
                    break;
                case 1048:
                case 706:
                case 65558:
                    name = "setTerrainType";
                    break;
                case 1051:
                    name = "setPinDetailVal";
                    break;
                case 1059:
                    name = "setPinDetailMax";
                    break;
                case 1052:
                    name = "setSmoothVal";
                    break;
                default:
                    return false;
            }
            if (valueCommand_ == 65558) {
                name = "forceTerrainType";
            }
            if ((valueCommand_ == 706 || valueCommand_ == 65558) &&
                (valueRevision_ != editor_->revision() ||
                 valueSelection_ != editor_->selection())) {
                return false;
            }
            accepted =
                editor_->command(name, std::array<std::string, 1>{edits_[0]->text}) == "True";
            if (accepted && valueCommand_ == 706) {
                accepted = editor_->action("setTerrainType");
            }
        } else {
            accepted = file(
                fileAction_, edits_[0]->text, edits_.size() > 1 ? edits_[1]->text : std::string{});
        }
        return accepted;
    };
    if (!ted.commitDialog(commit)) {
        restore();
        restoreScript();
        scriptPath_ = oldPath;
        scriptFormat_ = oldFormat;
        return false;
    }
    const auto wasRule = dialog_ == TerrainDialog::Rule;
    dialog_.reset();
    if (wasRule) {
        landscapeTab(landscapeTab());
    }
    return true;
}
}
