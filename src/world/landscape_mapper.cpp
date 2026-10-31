#include <opent1/world/landscape_mapper.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <locale>
#include <set>
#include <sstream>

namespace opent1::world {
namespace {
bool distribution(const LandscapeDistribution& d) {
    return std::isfinite(d.minimum) && std::isfinite(d.maximum) && std::isfinite(d.mean) &&
           std::isfinite(d.deviation) && d.minimum < d.mean && d.mean < d.maximum &&
           d.deviation > 0 && d.deviation < 1 && std::abs(d.minimum) < 1e9f &&
           std::abs(d.maximum) < 1e9f;
}

bool validRules(std::span<const LandscapeRule> rules) {
    return !rules.empty() && rules.size() <= 4096 && std::ranges::all_of(rules, [](const auto& r) {
        return r.group >= 0 && r.group < 16 && distribution(r.altitude) && distribution(r.slope) &&
               std::isfinite(r.altitudeWeight) && std::isfinite(r.slopeWeight) &&
               std::abs(r.altitudeWeight) < 1e6f && std::abs(r.slopeWeight) < 1e6f;
    });
}

Flt32 probability(const LandscapeDistribution& d, Flt32 deviation, Flt32 v) {
    if (v <= d.mean) {
        if (v <= d.minimum) {
            return .5f;
        }
        v = (d.mean - v) / (d.mean - d.minimum);
    } else {
        if (v >= d.maximum) {
            return .5f;
        }
        v = (v - d.mean) / (d.maximum - d.mean);
    }
    return v <= deviation ? .18f * v / deviation
                          : .18f + (v - deviation) / (1.f - deviation) * .32f;
}

constexpr std::array<std::array<UInt8, 4>, 8> orientation{{{0, 1, 2, 3},
                                                           {3, 0, 1, 2},
                                                           {1, 0, 3, 2},
                                                           {2, 1, 0, 3},
                                                           {3, 2, 1, 0},
                                                           {0, 3, 2, 1},
                                                           {2, 3, 0, 1},
                                                           {1, 2, 3, 0}}};

Point2I rotate(Point2I p, UInt32 r) {
    switch (r) {
        case 1:
            return {-p.y, p.x};
        case 2:
            return {-p.x, -p.y};
        case 3:
            return {p.y, -p.x};
        default:
            return p;
    }
}

bool coordinate(Point2I p, UInt32 width) {
    return p.x >= 0 && p.y >= 0 && UInt32(p.x) < width && UInt32(p.y) < width;
}
}

std::optional<std::vector<LandscapeRule>> readLandscapeRules(std::string_view text) {
    if (text.size() > 1024U * 1024U || text.find('\0') != text.npos) {
        return {};
    }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string line;
    Int32 version{}, count{};
    const auto marker = [&](std::string_view name) {
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line == name) {
                return true;
            }
        }
        return false;
    };
    if (!marker("VERSION") || !(input >> version) || version != 2 || !marker("NUMRULES") ||
        !(input >> count) || count < 1 || count > 4096) {
        return {};
    }
    std::getline(input, line);
    std::vector<LandscapeRule> rules;
    for (Int32 i = 0; i < count; ++i) {
        if (!std::getline(input, line) || line.size() > 1024) {
            return {};
        }
        std::ranges::replace(line, ',', ' ');
        std::istringstream record(line);
        record.imbue(std::locale::classic());
        LandscapeRule r;
        Int32 heights{}, slopes{};
        if (!(record >> r.group >> r.altitude.minimum >> r.altitude.maximum >> r.altitude.mean >>
              r.altitude.deviation >> r.altitudeWeight >> heights >> r.slope.minimum >>
              r.slope.maximum >> r.slope.mean >> r.slope.deviation >> r.slopeWeight >> slopes) ||
            (heights != 0 && heights != 1) || (slopes != 0 && slopes != 1)) {
            return {};
        }
        std::string tail;
        if (record >> tail) {
            return {};
        }
        r.adjustHeights = heights != 0;
        r.adjustSlopes = slopes != 0;
        rules.push_back(r);
    }
    if (!validRules(rules)) {
        return {};
    }
    return rules;
}

std::optional<std::string> writeLandscapeRules(std::span<const LandscapeRule> rules) {
    if (!validRules(rules)) {
        return {};
    }
    std::string text =
        std::format("VERSION\n2\n# Rule: typeL, alt(min,max,mean,sdev)F, altweightF, adjheightsL, "
                    "slope(min,max,mean,sdev)F, slopeweightF, adjslopesL\nNUMRULES\n{}\n",
                    rules.size());
    for (const auto& r : rules) {
        text += std::format(
            "{},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{}\n",
            r.group,
            r.altitude.minimum,
            r.altitude.maximum,
            r.altitude.mean,
            r.altitude.deviation,
            r.altitudeWeight,
            Int32(r.adjustHeights),
            r.slope.minimum,
            r.slope.maximum,
            r.slope.mean,
            r.slope.deviation,
            r.slopeWeight,
            Int32(r.adjustSlopes));
    }
    return text;
}

bool LandscapeTextureTable::valid() const {
    if ((version != 2 && version != 3) || types < 1 || types > 16 || corners.empty() ||
        corners.size() > 256 || picks.size() > 16U * 1024U * 1024U ||
        (version == 3 && descriptions.size() != types)) {
        return false;
    }
    for (const auto& d : descriptions) {
        if (d.size() > 31 || d.find('\0') != d.npos) {
            return false;
        }
    }
    for (auto c : corners) {
        if (!std::ranges::all_of(c, [&](auto x) {
                return x < types;
            })) {
            return false;
        }
    }
    const auto side = types + 1, count = side * side * side * side;
    if (combinations.size() != count || offsets.size() != count + 1 ||
        offsets.back() != picks.size()) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (combinations[i] >> types || offsets[i] > offsets[i + 1] ||
            offsets[i + 1] > picks.size()) {
            return false;
        }
        const auto start = offsets[i], end = offsets[i + 1];
        if (start == end) {
            continue;
        }
        if (end - start < 2 || picks[start] < 1 || picks[start] > 8 || picks[start + 1] < 1 ||
            end - start != 2U + picks[start] + 2U * picks[start + 1]) {
            return false;
        }
        for (UInt32 j = 0; j < picks[start]; ++j) {
            if (picks[start + 2 + j] > 7) {
                return false;
            }
        }
        UInt32 percent{};
        for (UInt32 j = start + 2 + picks[start]; j < end; j += 2) {
            if (picks[j] >= corners.size()) {
                return false;
            }
            percent += picks[j + 1];
        }
        if (picks[start + 1] > 1 && percent < 99) {
            return false;
        }
    }
    return true;
}

std::optional<LandscapeTextureTable> LandscapeTextureTable::load(std::span<const std::byte> bytes) {
    if (bytes.size() > 32U * 1024U * 1024U) {
        return {};
    }
    MemRWStream f(bytes);
    LandscapeTextureTable t;
    UInt32 textures{}, numPicks{};
    if (!f.read(t.version) || !f.read(t.types) || !f.read(textures) || !f.read(numPicks) ||
        t.types < 1 || t.types > 16 || textures < 1 || textures > 256 ||
        (t.version != 2 && t.version != 3)) {
        return {};
    }
    t.pickCount = numPicks;
    if (t.version == 3) {
        for (UInt32 i = 0; i < t.types; ++i) {
            std::array<std::byte, 32> name{};
            if (!f.readBytes(name)) {
                return {};
            }
            const auto end = std::ranges::find(name, std::byte{});
            if (end == name.end()) {
                return {};
            }
            t.descriptions.emplace_back(reinterpret_cast<const char*>(name.data()),
                                        std::size_t(end - name.begin()));
        }
    }
    t.corners.resize(textures);
    for (auto& c : t.corners) {
        for (auto& v : c) {
            if (!f.read(v)) {
                return {};
            }
        }
    }
    const auto side = t.types + 1, count = side * side * side * side;
    t.combinations.resize(count);
    t.offsets.resize(count + 1);
    for (auto& v : t.combinations) {
        if (!f.read(v)) {
            return {};
        }
    }
    for (auto& v : t.offsets) {
        if (!f.read(v)) {
            return {};
        }
    }
    const auto remain = bytes.subspan(std::size_t(f.getPosition()));
    t.picks.reserve(remain.size());
    for (auto v : remain) {
        t.picks.push_back(std::to_integer<UInt8>(v));
    }
    // numPicks is a legacy counter, not a byte extent; the terminal offset is authoritative.
    if (!t.valid()) {
        return {};
    }
    return t;
}

std::optional<std::vector<std::byte>> LandscapeTextureTable::encode() const {
    if (!valid()) {
        return {};
    }
    const auto side = types + 1, count = side * side * side * side;
    std::vector<std::byte> bytes(16 + (version == 3 ? types * 32 : 0) + corners.size() * 4 +
                                 (count * 2 + 1) * 4 + picks.size());
    MemRWStream f{std::span<std::byte>(bytes)};
    if (!f.write(version) || !f.write(types) || !f.write(UInt32(corners.size())) ||
        !f.write(pickCount)) {
        return {};
    }
    if (version == 3) {
        for (const auto& name : descriptions) {
            std::array<std::byte, 32> b{};
            for (std::size_t i = 0; i < name.size(); ++i) {
                b[i] = std::byte(UInt8(name[i]));
            }
            if (!f.writeBytes(b)) {
                return {};
            }
        }
    }
    for (auto c : corners) {
        for (auto v : c) {
            if (!f.write(v)) {
                return {};
            }
        }
    }
    for (auto v : combinations) {
        if (!f.write(v)) {
            return {};
        }
    }
    for (auto v : offsets) {
        if (!f.write(v)) {
            return {};
        }
    }
    for (auto v : picks) {
        if (!f.write(v)) {
            return {};
        }
    }
    return bytes;
}

bool LandscapeMapper::setRules(std::vector<LandscapeRule> r) {
    if (!validRules(r)) {
        return false;
    }
    rules_ = std::move(r);
    return true;
}

bool LandscapeMapper::setTextures(LandscapeTextureTable t) {
    if (!t.valid()) {
        return false;
    }
    textures_ = std::move(t);
    return true;
}

bool LandscapeTextureRecipe::addType(char tag, std::string description) {
    if (tag >= 'a' && tag <= 'z') {
        tag = char(tag - 32);
    }
    if (types.size() >= 8 || tag < 33 || description.size() > 31 ||
        description.find('\0') != description.npos ||
        std::ranges::any_of(types, [&](const auto& t) {
            return t.first == tag;
        })) {
        return false;
    }
    types.emplace_back(tag, std::move(description));
    return true;
}

bool LandscapeTextureRecipe::addTexture(std::string_view file,
                                        std::string_view sides,
                                        UInt8 rotations,
                                        UInt8 percent,
                                        std::string_view physical,
                                        Flt32 elasticity,
                                        Flt32 friction) {
    if (file.empty() || file.size() > 31 || file.find('\0') != file.npos || sides.size() != 4 ||
        !rotations || textures.size() >= 255 || !std::isfinite(elasticity) ||
        !std::isfinite(friction) || std::abs(elasticity) > 1e6f || std::abs(friction) > 1e6f) {
        return false;
    }
    LandscapeTextureDefinition d;
    d.material.flags = 0x103;
    d.material.mapFile = file;
    d.material.elasticity = elasticity;
    d.material.friction = friction;
    d.material.useDefaultProperties = false;
    d.rotations = rotations;
    d.percentage = percent;
    for (UInt32 i = 0; i < 4; ++i) {
        auto tag = sides[i];
        if (tag >= 'a' && tag <= 'z') {
            tag = char(tag - 32);
        }
        const auto at = std::ranges::find(types, tag, &std::pair<char, std::string>::first);
        if (at == types.end()) {
            return false;
        }
        d.corners[3 - i] = UInt8(at - types.begin());
    } // native addTerrainTexture reverses FOURCC sides
    constexpr std::array<std::string_view, 15> names{"DEFAULT",
                                                     "CONCRETE",
                                                     "CARPET",
                                                     "METAL",
                                                     "GLASS",
                                                     "PLASTIC",
                                                     "WOOD",
                                                     "MARBLE",
                                                     "SNOW",
                                                     "ICE",
                                                     "SAND",
                                                     "MUD",
                                                     "STONE",
                                                     "SOFTEARTH",
                                                     "PACKEDEARTH"};
    std::string name(physical);
    for (auto& c : name) {
        if (c >= 'a' && c <= 'z') {
            c = char(c - 32);
        }
    }
    const auto found = std::ranges::find(names, name);
    d.material.surfaceType = found == names.end() ? 0 : Int32(found - names.begin());
    textures.push_back(std::move(d));
    return true;
}

std::optional<LandscapeTextureTable> LandscapeTextureRecipe::build() const {
    if (types.empty() || types.size() > 8 || textures.empty() || textures.size() > 255) {
        return {};
    }
    LandscapeTextureTable t;
    t.types = UInt32(types.size());
    for (const auto& type : types) {
        t.descriptions.push_back(type.second);
    }

    struct State {
        UInt8 id, flag, percent;
        std::array<UInt8, 4> corners;
    };

    std::vector<State> states;
    for (UInt32 i = 0; i < textures.size(); ++i) {
        const auto& d = textures[i];
        if (!d.rotations || !std::ranges::all_of(d.corners, [&](auto c) {
                return c < t.types;
            })) {
            return {};
        }
        t.corners.push_back(d.corners);
        for (UInt32 j = 0; j < 8; ++j) {
            if (d.rotations & (1U << j)) {
                State s{UInt8(i), UInt8(j), d.percentage, {}};
                for (UInt32 c = 0; c < 4; ++c) {
                    s.corners[c] = d.corners[orientation[j][c]];
                }
                states.push_back(s);
            }
        }
    }
    const auto base = t.types + 1, count = base * base * base * base;
    t.offsets.push_back(0);
    for (UInt32 code = 0; code < count; ++code) {
        auto value = code;
        std::array<UInt32, 4> corners{};
        for (Int32 c = 3; c >= 0; --c) {
            corners[std::size_t(c)] = value % base;
            value /= base;
        }
        UInt32 combinations{}, flips{};
        std::vector<std::pair<UInt8, UInt8>> picks;
        for (const auto& s : states) {
            bool match = true, complete = true;
            for (UInt32 c = 0; c < 4; ++c) {
                if (corners[c] && corners[c] - 1 != s.corners[c]) {
                    match = false;
                }
                if (!corners[c]) {
                    complete = false;
                }
            }
            if (match) {
                for (UInt32 c = 0; c < 4; ++c) {
                    if (!corners[c]) {
                        combinations |= 1U << s.corners[c];
                    }
                }
            }
            if (match && complete) {
                flips |= 1U << s.flag;
                if (std::ranges::none_of(picks, [&](auto p) {
                        return p.first == s.id;
                    })) {
                    picks.emplace_back(s.id, s.percent);
                }
            }
        }
        t.combinations.push_back(combinations);
        if (!picks.empty()) {
            t.picks.push_back(UInt8(std::popcount(flips)));
            t.picks.push_back(UInt8(picks.size()));
            for (UInt32 j = 0; j < 8; ++j) {
                if (flips & (1U << j)) {
                    t.picks.push_back(UInt8(j));
                }
            }
            for (auto p : picks) {
                t.picks.push_back(p.first);
                t.picks.push_back(p.second);
            }
        }
        t.offsets.push_back(UInt32(t.picks.size()));
    }
    if (!t.valid()) {
        return {};
    }
    return t;
}

std::optional<std::vector<std::byte>> LandscapeTextureRecipe::materialList() const {
    if (!build()) {
        return {};
    }
    const auto body = UInt32(2 + 16 + 4 + 8 + textures.size() * 60);
    std::vector<std::byte> bytes(8 + body);
    MemRWStream f{std::span<std::byte>(bytes)};
    if (!f.write(makeFourCC('P', 'E', 'R', 'S')) || !f.write(body) || !f.write(Int16(16)) ||
        !f.writeBytes(std::as_bytes(std::span("TS::MaterialList", 16))) || !f.write(Int32(3)) ||
        !f.write(Int32(1)) || !f.write(Int32(textures.size()))) {
        return {};
    }
    for (const auto& d : textures) {
        const auto& m = d.material;
        std::array<std::byte, 32> name{};
        for (std::size_t i = 0; i < m.mapFile.size(); ++i) {
            name[i] = std::byte(UInt8(m.mapFile[i]));
        }
        if (!f.write(m.flags) || !f.write(m.alpha) || !f.write(m.index) ||
            !f.writeBytes(std::as_bytes(std::span(m.rgb))) || !f.writeBytes(name) ||
            !f.write(m.surfaceType) || !f.write(m.elasticity) || !f.write(m.friction)) {
            return {};
        }
    }
    return bytes;
}

bool LandscapeMapper::map(UInt32 width,
                          Int32 scale,
                          std::span<const Flt32> heights,
                          std::vector<TerrainGridMaterial>& materials,
                          std::span<const LandscapeCorner> forced,
                          bool local,
                          Random& random) const {
    if (!std::has_single_bit(width) || width > 2048 ||
        heights.size() != std::size_t(width) * width || materials.size() != heights.size() ||
        !textures_.valid() || !validRules(rules_)) {
        return false;
    }
    const auto mask = width - 1, base = textures_.types + 1;
    const std::array<UInt32, 4> place{1, base, base * base, base * base * base};
    std::vector<UInt32> rotations(heights.size());
    std::vector<UInt8> corners(heights.size());
    const auto index = [&](Int32 x, Int32 y) {
        return (UInt32(y) & mask) * width + (UInt32(x) & mask);
    };
    const auto seed = [&](Point2I p, UInt8 type) {
        ++type;
        corners[index(p.x, p.y)] = type;
        const std::array<UInt32, 4> at{
            index(p.x, p.y), index(p.x - 1, p.y), index(p.x - 1, p.y - 1), index(p.x, p.y - 1)};
        for (UInt32 k = 0; k < 4; ++k) {
            auto& v = rotations[at[k]];
            v = v - (v / place[k] % base) * place[k] + type * place[k];
        }
    };
    for (UInt32 y = 0; y < width; ++y) {
        for (UInt32 x = 0; x < width; ++x) {
            auto& m = materials[std::size_t(y) * width + x];
            if (m.index >= textures_.corners.size() && (local || (m.flags & 0xc0))) {
                return false;
            }
            if (local || (m.flags & 0xc0)) {
                const auto& c = textures_.corners[m.index];
                const auto& order = orientation[m.flags & (local || !(m.flags & 0x80) ? 3 : 7)];
                seed({Int32(x), Int32(y)}, c[order[3]]);
                if (!(m.flags & 0x80) || local) {
                    seed({Int32(x + 1), Int32(y)}, c[order[2]]);
                    seed({Int32(x), Int32(y + 1)}, c[order[0]]);
                    seed({Int32(x + 1), Int32(y + 1)}, c[order[1]]);
                } else {
                    m.flags = 0x80;
                    m.index = 0xff;
                }
            } else {
                m.index = 0xff;
            }
            if (local) {
                m.index = m.flags = 0xff;
            } // native local applicator preserves this quirk
        }
    }
    for (const auto& c : forced) {
        if (!coordinate(c.point, width) || c.type >= textures_.types) {
            return false;
        }
        seed(c.point, c.type);
    }
    for (UInt32 y = 0; y < width; ++y) {
        for (UInt32 x = 0; x < width; ++x) {
            const auto at = std::size_t(y) * width + x;
            if (corners[at]) {
                continue;
            }
            const std::array<UInt32, 4> adjacent{index(Int32(x) - 1, Int32(y)),
                                                 UInt32(at),
                                                 index(Int32(x) - 1, Int32(y) - 1),
                                                 index(Int32(x), Int32(y) - 1)};
            UInt32 combos = 0xffffffffU;
            for (auto a : adjacent) {
                if (rotations[a] >= textures_.combinations.size()) {
                    return false;
                }
                combos &= textures_.combinations[rotations[a]];
            }
            UInt8 type{};
            float high = -1;
            float minimum = 1e20f, maximum = -1e20f;
            for (Int32 j = -1; j <= 1; ++j) {
                for (Int32 i = -1; i <= 1; ++i) {
                    const auto h = heights[index(Int32(x) + i, Int32(y) + j)];
                    minimum = std::min(minimum, h);
                    maximum = std::max(maximum, h);
                }
            }
            const auto alt = (maximum + minimum) * .5f,
                       slope = (maximum - minimum) * .5f / Flt32(1U << scale);
            for (const auto& r : rules_) {
                if (!(combos & (1U << r.group))) {
                    continue;
                }
                const auto score =
                    ((r.adjustHeights && (alt < r.altitude.minimum || alt > r.altitude.maximum)) ||
                     (r.adjustSlopes && (slope < r.slope.minimum || slope > r.slope.maximum)))
                        ? 0.f
                        : r.altitudeWeight *
                                  (1 - 2 * probability(r.altitude, r.altitude.deviation, alt)) +
                              r.slopeWeight *
                                  (1 - 2 * probability(r.slope, r.altitude.deviation, slope));
                if (score > high) {
                    high = score;
                    type = UInt8(r.group + 1);
                }
            }
            rotations[adjacent[0]] += type * place[1];
            rotations[adjacent[1]] += type;
            rotations[adjacent[2]] += type * place[2];
            rotations[adjacent[3]] += type * place[3];
        }
    }
    for (std::size_t i = 0; i < materials.size(); ++i) {
        auto& m = materials[i];
        if (m.flags & 0x40) {
            continue;
        }
        const auto code = rotations[i];
        if (code >= textures_.combinations.size()) {
            return false;
        }
        const auto start = textures_.offsets[code], end = textures_.offsets[code + 1];
        if (end - start < 2) {
            return false;
        }
        const auto flips = textures_.picks[start], textures = textures_.picks[start + 1];
        UInt32 flip = flips > 1 ? UInt32(random.getInt(0, flips - 1)) : 0;
        m.flags = UInt8((m.flags & ~7U) | textures_.picks[start + 2 + flip]);
        UInt32 pick = start + 2 + flips;
        if (textures > 1) {
            auto percent = random.getInt(0, 99);
            while ((percent -= textures_.picks[pick + 1]) > 0) {
                pick += 2;
                if (pick + 1 >= end) {
                    return false;
                }
            }
        }
        m.index = textures_.picks[pick];
    }
    return true;
}

bool LandscapeMapper::apply(TerrainDocument& d, std::span<const LandscapeCorner> forced) {
    if (!d.valid() || d.grid.size.x != d.grid.size.y) {
        return false;
    }
    auto next = d;
    auto random = random_;
    const auto bw = d.blocks[0].width;
    const UInt32 across = d.grid.pattern == 0   ? 1
                          : d.grid.pattern == 2 ? 4
                                                : UInt32(d.grid.size.x),
                 width = bw * across;
    if (!std::has_single_bit(width) || width > 2048) {
        return false;
    }
    for (UInt32 pass = 0; pass < (d.grid.pattern == 2 ? 2U : 1U); ++pass) {
        const UInt32 start = pass * 16;
        if (next.blocks.size() < start + across * across) {
            return false;
        }
        std::vector<float> heights(std::size_t(width) * width);
        std::vector<TerrainGridMaterial> materials(heights.size());
        for (UInt32 b = 0; b < across * across; ++b) {
            auto& block = next.blocks[start + b];
            for (UInt32 y = 0; y < bw; ++y) {
                for (UInt32 x = 0; x < bw; ++x) {
                    const auto at =
                        std::size_t(b / across * bw + y) * width + (b % across * bw + x);
                    heights[at] = block.heights[std::size_t(y) * (bw + 1) + x];
                    materials[at] = block.materials[std::size_t(y) * bw + x];
                }
            }
        }
        std::vector<LandscapeCorner> points;
        const auto center = pass ? (d.grid.size.x - 4) / 2 * Int32(bw) : 0;
        for (auto c : forced) {
            if (d.grid.pattern == 0) {
                c.point.x &= Int32(bw - 1);
                c.point.y &= Int32(bw - 1);
            }
            c.point.x -= center;
            c.point.y -= center;
            if (coordinate(c.point, width)) {
                points.push_back(c);
            }
        }
        if (!map(width, d.grid.scale, heights, materials, points, false, random)) {
            return false;
        }
        for (UInt32 b = 0; b < across * across; ++b) {
            auto& block = next.blocks[start + b];
            for (UInt32 y = 0; y < bw; ++y) {
                for (UInt32 x = 0; x < bw; ++x) {
                    block.materials[std::size_t(y) * bw + x] =
                        materials[std::size_t(b / across * bw + y) * width + b % across * bw + x];
                }
            }
        }
    }
    d = std::move(next);
    random_ = random;
    return true;
}

bool LandscapeMapper::applyLocal(TerrainDocument& d,
                                 gfx::RectI area,
                                 std::span<const LandscapeCorner> forced) {
    if (!d.valid() || !area.valid() || area.width() != area.height() || area.width() > 2048 ||
        !std::has_single_bit(UInt32(area.width())) || area.left < 0 || area.top < 0) {
        return false;
    }
    const auto bw = d.blocks[0].width, width = UInt32(area.width());
    if (area.right >= Int64(d.grid.size.x) * bw || area.bottom >= Int64(d.grid.size.y) * bw) {
        return false;
    }
    auto next = d;
    auto random = random_;
    std::vector<float> heights(std::size_t(width) * width);
    std::vector<TerrainGridMaterial> materials(heights.size());
    std::vector<std::pair<std::size_t, std::size_t>> addresses;
    for (Int32 y = area.top; y <= area.bottom; ++y) {
        for (Int32 x = area.left; x <= area.right; ++x) {
            const auto id =
                d.grid.blockMap[std::size_t(UInt32(y) / bw) * d.grid.size.x + UInt32(x) / bw];
            const auto block = std::ranges::find(d.grid.blocks, id, &TerrainGridBlockEntry::id);
            if (block == d.grid.blocks.end()) {
                return false;
            }
            const auto i = std::size_t(block - d.grid.blocks.begin()),
                       at = std::size_t(UInt32(y) % bw) * bw + UInt32(x) % bw;
            addresses.emplace_back(i, at);
            materials[addresses.size() - 1] = d.blocks[i].materials[at];
            heights[addresses.size() - 1] =
                d.blocks[i].heights[std::size_t(UInt32(y) % bw) * (bw + 1) + UInt32(x) % bw];
        }
    }
    if (!map(width, d.grid.scale, heights, materials, forced, true, random)) {
        return false;
    }
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        next.blocks[addresses[i].first].materials[addresses[i].second] = materials[i];
    }
    d = std::move(next);
    random_ = random;
    return true;
}

bool LandscapeMapper::stamp(TerrainDocument& d,
                            UInt32 block,
                            Point2I origin,
                            UInt32 rotation,
                            const LandscapeStamp& stamp) {
    if (!d.valid() || block >= d.blocks.size() || rotation > 3 || stamp.flat.size() > 65536 ||
        stamp.textures.size() > 65536 || stamp.clampDetail > 7 ||
        std::abs(Int64(origin.x)) > 1'000'000 || std::abs(Int64(origin.y)) > 1'000'000) {
        return false;
    }
    auto next = d;
    auto& b = next.blocks[block];
    const auto width = b.width;
    const auto point = [&](Point2I p) {
        if (std::abs(Int64(p.x)) > 1'000'000 || std::abs(Int64(p.y)) > 1'000'000) {
            return Point2I{-1, -1};
        }
        p = rotate(p, rotation);
        p.x += origin.x;
        p.y += origin.y;
        return p;
    };
    float average{};
    std::vector<Point2I> flat;
    for (auto p : stamp.flat) {
        p = point(p);
        if (!coordinate(p, width)) {
            return false;
        }
        flat.push_back(p);
        average += b.heights[std::size_t(p.y) * (width + 1) + p.x];
    }
    if (!flat.empty()) {
        average /= Flt32(flat.size());
    }
    for (auto p : flat) {
        for (UInt32 y = 0; y < 2; ++y) {
            for (UInt32 x = 0; x < 2; ++x) {
                b.heights[(std::size_t(p.y) + y) * (width + 1) + UInt32(p.x) + x] = average;
            }
        }
    }
    constexpr std::array<Flt32, 8> deviations{0, 3, 12, 30, 60, 130, 300, 700};
    for (auto p : flat) {
        const auto height = b.heights[std::size_t(p.y) * (width + 1) + p.x];
        for (UInt32 level = 1; level <= stamp.clampDetail; ++level) {
            const auto step = 1U << level, x = UInt32(p.x) & ~(step - 1),
                       y = UInt32(p.y) & ~(step - 1);
            for (UInt32 j = 0; j < 2; ++j) {
                for (UInt32 i = 0; i < 2; ++i) {
                    if (x + i * step > width || y + j * step > width) {
                        continue;
                    }
                    auto& h = b.heights[std::size_t(y + j * step) * (width + 1) + x + i * step];
                    h = std::clamp(h, height - deviations[level], height + deviations[level]);
                }
            }
        }
    }
    constexpr std::array<UInt8, 4> rotations{0, 1, 6, 7};
    for (auto t : stamp.textures) {
        const auto p = point(t.point);
        if (!coordinate(p, width) ||
            (!textures_.corners.empty() && t.material >= textures_.corners.size())) {
            return false;
        }
        auto flags = UInt8((t.flags & 6) ^ rotations[rotation]);
        if ((flags & 1) && (t.flags & 1)) {
            flags ^= 7;
        } else {
            flags |= t.flags & 1;
        }
        flags |= t.flags & ~7U;
        const auto hole = ((t.flags >> 3) & 7) != 0;
        b.materials[std::size_t(p.y) * width + p.x] = {hole ? t.flags : flags,
                                                       UInt8(hole ? 255 : t.material)};
    }
    if (!next.rebuildHeightRanges() || !next.valid()) {
        return false;
    }
    d = std::move(next);
    return true;
}
} // namespace opent1::world
