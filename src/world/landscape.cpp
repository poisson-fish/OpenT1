#include <opent1/world/landscape.hpp>
#include <opent1/math/random.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace opent1::world {
std::optional<LandscapeScriptFile> readLandscapeScript(std::span<const std::byte> bytes,
                                                       std::size_t maxBytes) {
    if (maxBytes > 1024U * 1024U || bytes.size() > maxBytes + 5) {
        return {};
    }
    auto format = LandscapeScriptFormat::RawText;
    if (bytes.size() >= 4) {
        UInt32 count{};
        for (unsigned i = 0; i < 4; ++i) {
            count |= UInt32(bytes[i]) << (i * 8);
        }
        if (count && count <= maxBytes + 1 && std::size_t(count) + 4 == bytes.size() &&
            bytes.back() == std::byte{}) {
            format = LandscapeScriptFormat::NativeCounted;
            bytes = bytes.subspan(4, count - 1);
        }
    }
    if (bytes.size() > maxBytes || std::ranges::any_of(bytes, [](std::byte b) {
            const auto c = UInt8(b);
            return c == 0 ||
                   (c < 32 && c != '\r' && c != '\n' && c != '\t' && c != '\v' && c != '\f');
        })) {
        return {};
    }
    std::string text;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto c = char(bytes[i]);
        if (c == '\r') {
            text += '\n';
            if (i + 1 < bytes.size() && bytes[i + 1] == std::byte{'\n'}) {
                ++i;
            }
        } else {
            text += c;
        }
    }
    return LandscapeScriptFile{std::move(text), format};
}

std::optional<std::vector<std::byte>> writeLandscapeScript(std::string_view text,
                                                           LandscapeScriptFormat format) {
    if (text.size() > 65536 || text.find('\0') != text.npos ||
        (format != LandscapeScriptFormat::RawText &&
         format != LandscapeScriptFormat::NativeCounted)) {
        return {};
    }
    std::vector<std::byte> result;
    if (format == LandscapeScriptFormat::NativeCounted) {
        const auto count = UInt32(text.size() + 1);
        for (unsigned i = 0; i < 4; ++i) {
            result.push_back(std::byte(count >> (i * 8)));
        }
    }
    const auto bytes = std::as_bytes(std::span(text));
    result.insert(result.end(), bytes.begin(), bytes.end());
    if (format == LandscapeScriptFormat::NativeCounted) {
        result.push_back(std::byte{});
    }
    return result;
}

namespace {
struct Matrix {
    int size;
    std::vector<float> values;
    float* real;

    explicit Matrix(int n) : size(n), values(std::size_t(n) * n), real(values.data()) {}

    Matrix(const Matrix& m) : size(m.size), values(m.values), real(values.data()) {}
};

struct MatrixStack : std::vector<std::unique_ptr<Matrix>> {
    void push_front(std::unique_ptr<Matrix> p) {
        insert(begin(), std::move(p));
    }

    void pop_front() {
        erase(begin());
    }
};

struct NativeLandscape {
    MatrixStack stack;
    Random rand;

    enum { EXEC_FAILURE = 0, EXEC_SUCCESS = 1 };

    int mExecStatus{EXEC_SUCCESS};
    char mExecString[256]{};
    NativeLandscape() = default;

    NativeLandscape(const NativeLandscape& n) : rand(n.rand) {
        for (const auto& m : n.stack) {
            stack.push_back(std::make_unique<Matrix>(*m));
        }
    }

    void pop() {
        if (!stack.empty()) {
            stack.pop_front();
        }
    }

    void swap() {
        if (stack.size() >= 2) {
            std::swap(stack[0], stack[1]);
        }
    }

    void dup() {
        if (!stack.empty()) {
            stack.push_front(std::make_unique<Matrix>(*stack[0]));
        }
    }

    void size(int n) {
        if (!stack.empty() && n > stack[0]->size) {
            auto m = std::make_unique<Matrix>(n);
            const auto old = stack[0]->size, scale = n / old;
            for (int y = 0; y < n; ++y) {
                for (int x = 0; x < n; ++x) {
                    m->real[y * n + x] = stack[0]->real[y / scale * old + x / scale];
                }
            }
            stack[0] = std::move(m);
        }
    }

    void add(float);
    void sub(float);
    void mul(float);
    void div(float);
    void exp(float);
    void neg();
    void clr(float);
    void normalize(float, float);
    void addStack(int = 1);
    void subStack(int = 1);
    void overlay(int, float);
    void blend(int, int);
    void flipX();
    void flipY();
    void fillNormal(float);
    void fillNormalHalf(float);
    void terrain(int, float);
    void plasma(int, float);
    void wrap();
    void tile();
    void alphablend(int, int);
    void smooth(float, float);
    void floor(float, float, float);
    void ceil(float, float, float);
    void rot();
    void curve(float, int);
    void slope(float, int);
    void shaveArea(float, float, float);
    void clamp(int, float);
    void mask(int, float);
    void diff();
    void fft(int);
    void clipMin(float);
    void clipMax(float);
};

constexpr double M_2PI = 6.283185307179586476925286766559;

constexpr int square(int value) {
    return value * value;
}

#define GOOD_TRIG
#include "landscape_trig.inc"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4267 4018)
#endif
#include "landscape_algorithms.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#undef minf
#undef maxf
#undef absf
#undef modf
#undef El
#undef Elmod
#undef Elclip

bool dimension(double n) {
    return n >= 4 && n <= 512 && n == std::floor(n) && std::has_single_bit(UInt32(n));
}

struct Operation {
    std::string_view name;
    LandscapeOpcode code;
    std::string_view types;
};

constexpr auto operations =
    std::to_array<Operation>({{"SEED", LandscapeOpcode::Seed, "i"},
                              {"LOAD", LandscapeOpcode::Load, "s"},
                              {"LOADM", LandscapeOpcode::LoadMax, "sf"},
                              {"SAVE", LandscapeOpcode::Save, "s"},
                              {"CLEAR", LandscapeOpcode::Clear, ""},
                              {"PUSH", LandscapeOpcode::Push, "i"},
                              {"POP", LandscapeOpcode::Pop, ""},
                              {"GET", LandscapeOpcode::Get, "i"},
                              {"SWAP", LandscapeOpcode::Swap, ""},
                              {"DUP", LandscapeOpcode::Dup, ""},
                              {"SIZE", LandscapeOpcode::Size, "i"},
                              {"ADD", LandscapeOpcode::Add, "f"},
                              {"SUB", LandscapeOpcode::Sub, "f"},
                              {"MUL", LandscapeOpcode::Mul, "f"},
                              {"DIV", LandscapeOpcode::Div, "f"},
                              {"EXP", LandscapeOpcode::Exp, "f"},
                              {"NEG", LandscapeOpcode::Neg, ""},
                              {"CLR", LandscapeOpcode::Clr, "f"},
                              {"DIFF", LandscapeOpcode::Diff, ""},
                              {"NORMALIZE", LandscapeOpcode::Normalize, "ff"},
                              {"ADD_STACK", LandscapeOpcode::AddStack, "i"},
                              {"SUB_STACK", LandscapeOpcode::SubStack, "i"},
                              {"TERRAIN", LandscapeOpcode::Terrain, "if"},
                              {"PLASMA", LandscapeOpcode::Plasma, "if"},
                              {"CRATER", LandscapeOpcode::Crater, ""},
                              {"PEAK", LandscapeOpcode::Peak, ""},
                              {"ROT", LandscapeOpcode::Rot, ""},
                              {"RING", LandscapeOpcode::Ring, ""},
                              {"FILLBASIN", LandscapeOpcode::FillBasin, ""},
                              {"OVERLAY", LandscapeOpcode::Overlay, "if"},
                              {"BLEND", LandscapeOpcode::Blend, "ii"},
                              {"FLIPX", LandscapeOpcode::FlipX, ""},
                              {"FLIPY", LandscapeOpcode::FlipY, ""},
                              {"LPFILTER", LandscapeOpcode::LowPass, ""},
                              {"HPFILTER", LandscapeOpcode::HighPass, ""},
                              {"BPFILTER", LandscapeOpcode::BandPass, ""},
                              {"BRFILTER", LandscapeOpcode::BandReject, ""},
                              {"FFLP", LandscapeOpcode::FastLowPass, ""},
                              {"FFHP", LandscapeOpcode::FastHighPass, ""},
                              {"FFBP", LandscapeOpcode::FastBandPass, ""},
                              {"FFBR", LandscapeOpcode::FastBandReject, ""},
                              {"FFT", LandscapeOpcode::Fft, "i"},
                              {"SMOOTH", LandscapeOpcode::Smooth, "ff"},
                              {"SLOPE", LandscapeOpcode::Slope, "fi"},
                              {"SHAVE", LandscapeOpcode::Shave, "fff"},
                              {"CURVE", LandscapeOpcode::Curve, "fi"},
                              {"FLOOR", LandscapeOpcode::Floor, "fff"},
                              {"CEIL", LandscapeOpcode::Ceil, "fff"},
                              {"FILL_N", LandscapeOpcode::FillNormal, "f"},
                              {"ALPHABLEND", LandscapeOpcode::AlphaBlend, "ii"},
                              {"TILE", LandscapeOpcode::Tile, ""},
                              {"WRAP", LandscapeOpcode::Wrap, ""},
                              {"CLAMP", LandscapeOpcode::Clamp, "if"},
                              {"MASK", LandscapeOpcode::Mask, "if"},
                              {"CLIPMIN", LandscapeOpcode::ClipMin, "f"},
                              {"CLIPMAX", LandscapeOpcode::ClipMax, "f"}});

bool nativeNoop(LandscapeOpcode op) {
    const auto n = UInt32(op);
    return op == LandscapeOpcode::Crater || op == LandscapeOpcode::Peak ||
           op == LandscapeOpcode::Ring || op == LandscapeOpcode::FillBasin ||
           (n >= 600 && n <= 607);
}

std::string upper(std::string s) {
    for (auto& c : s) {
        if (c >= 'a' && c <= 'z') {
            c = char(c - 32);
        }
    }
    return s;
}
} // namespace

struct Landscape::Impl {
    NativeLandscape native;
};

Landscape::Landscape(UInt32 size) : impl_(std::make_unique<Impl>()) {
    if (!dimension(size)) {
        throw std::invalid_argument("Landscape size must be a power of two from 4 to 512");
    }
    impl_->native.stack.push_front(std::make_unique<Matrix>(Int32(size)));
}

Landscape::~Landscape() = default;

Landscape::Landscape(const Landscape& l)
    : impl_(std::make_unique<Impl>(*l.impl_)), error_(l.error_), outputs_(l.outputs_) {}

Landscape& Landscape::operator=(const Landscape& l) {
    if (&l != this) {
        Landscape copy(l);
        *this = std::move(copy);
    }
    return *this;
}

Landscape::Landscape(Landscape&&) noexcept = default;
Landscape& Landscape::operator=(Landscape&&) noexcept = default;

UInt32 Landscape::seed() const {
    return impl_->native.rand.getSeed();
}

std::vector<LandscapeMatrix> Landscape::stack() const {
    std::vector<LandscapeMatrix> result;
    for (const auto& m : impl_->native.stack) {
        result.push_back({UInt32(m->size), m->values});
    }
    return result;
}

bool Landscape::importHeightfield(const LandscapeMatrix& m) {
    if (!dimension(m.size) || m.values.size() != std::size_t(m.size) * m.size ||
        impl_->native.stack.size() >= 32 || !std::ranges::all_of(m.values, [](auto f) {
            return std::isfinite(f) && std::abs(f) <= 1e7f;
        })) {
        error_ = "Invalid landscape heightfield";
        return false;
    }
    auto native = std::make_unique<Matrix>(Int32(m.size));
    native->values = m.values;
    native->real = native->values.data();
    impl_->native.stack.insert(impl_->native.stack.begin(), std::move(native));
    error_.clear();
    return true;
}

bool Landscape::importBitmap(const Bitmap& b, Flt32 maxHeight) {
    if (!b.valid() || b.bitDepth != 8 || b.width != b.height || !dimension(b.width) ||
        !std::isfinite(maxHeight) || std::abs(maxHeight) > 1e7f ||
        impl_->native.stack.size() >= 32) {
        error_ = "Invalid landscape bitmap";
        return false;
    }
    auto m = std::make_unique<Matrix>(b.width);
    for (Int32 y = 0; y < b.height; ++y) {
        const auto row = b.row(y);
        for (Int32 x = 0; x < b.width; ++x) {
            m->real[y * b.width + x] =
                Flt32(maxHeight * std::to_integer<UInt8>(row[std::size_t(x)]) / 255.0);
        }
    }
    impl_->native.stack.insert(impl_->native.stack.begin(), std::move(m));
    error_.clear();
    return true;
}

std::optional<Bitmap> Landscape::bitmap(bool normalize) const {
    if (impl_->native.stack.empty()) {
        return {};
    }
    NativeLandscape n = impl_->native;
    if (normalize) {
        n.normalize(.04f, .96f);
    }
    const auto& m = *n.stack[0];
    auto b = Bitmap::create(m.size, m.size);
    if (!b) {
        return {};
    }
    for (Int32 y = 0; y < m.size; ++y) {
        auto row = b->row(y);
        for (Int32 x = 0; x < m.size; ++x) {
            const auto v = double(m.real[y * m.size + x]) * 255.0;
            if (!std::isfinite(v) || v < double(std::numeric_limits<Int32>::min()) ||
                v > double(std::numeric_limits<Int32>::max())) {
                return {};
            }
            row[std::size_t(x)] = std::byte(UInt32(Int32(v)) & 255);
        }
    }
    return std::move(*b);
}

bool Landscape::run(const LandscapeCommand& c, const Loader& loader) {
    const auto at = std::ranges::find(operations, c.opcode, &Operation::code);
    const auto fail = [&](std::string_view s) {
        error_ = s;
        return false;
    };
    if (at == std::end(operations)) {
        return fail("Unknown landscape opcode");
    }
    std::size_t index{};
    for (auto type : at->types) {
        if (type == 's') {
            if (c.name.empty() || c.name.size() > 255 || c.name.find('\0') != c.name.npos) {
                return fail("Invalid landscape filename");
            }
            continue;
        }
        if (index >= c.arguments.size() || !std::isfinite(c.arguments[index]) ||
            std::abs(c.arguments[index]) >
                (c.opcode == LandscapeOpcode::Seed ? 4294967295. : 1e9) ||
            (type == 'i' && c.arguments[index] != std::floor(c.arguments[index]))) {
            return fail("Invalid landscape argument");
        }
        ++index;
    }
    if (index != c.arguments.size()) {
        return fail("Incorrect landscape argument count");
    }
    const auto a = [&](std::size_t i) {
        return Flt32(c.arguments[i]);
    };
    const auto i = [&](std::size_t k) {
        return Int32(c.arguments[k]);
    };
    auto& n = impl_->native;
    auto& s = n.stack;
    n.mExecStatus = 1;
    n.mExecString[0] = 0;
    using O = LandscapeOpcode;
    const auto op = c.opcode;
    if (nativeNoop(op)) {
        return true;
    }
    if (op == O::Seed) {
        if (c.arguments[0] < 0) {
            return fail("Invalid landscape seed");
        }
        n.rand.setSeed(UInt32(c.arguments[0]), false);
        return true;
    }
    if (op == O::Clear) {
        s.clear();
        return true;
    }
    if (op == O::Push || op == O::Terrain || op == O::Plasma) {
        if (!dimension(c.arguments[0]) || s.size() >= 30) {
            return fail("Landscape stack/size limit");
        }
        if (op == O::Push) {
            s.push_front(std::make_unique<Matrix>(i(0)));
        } else if (op == O::Terrain) {
            n.terrain(std::min(i(0), 256), a(1));
            n.size(i(0));
        } else {
            n.plasma(std::min(i(0), 256), a(1));
            n.size(i(0));
        }
    } else if (op == O::Load || op == O::LoadMax) {
        if (!loader) {
            return fail("No landscape bitmap loader");
        }
        auto b = loader(c.name);
        if (!b || !importBitmap(*b, op == O::Load ? 1 : a(0))) {
            return fail("Landscape load failed");
        }
    } else if (op == O::Pop) {
        n.pop();
    } else {
        if (s.empty()) {
            return fail("Landscape stack empty");
        }
        if ((op == O::Swap || op == O::AddStack || op == O::SubStack || op == O::Fft ||
             op == O::FillNormal || op == O::Shave || op == O::Slope) &&
            s.size() < 2) {
            return fail("Landscape stack too small");
        }
        if ((op == O::Fft || op == O::FillNormal || op == O::Shave) && s[0]->size != s[1]->size) {
            return fail("Landscape matrix size mismatch");
        }
        if (s.size() >= 31) {
            return fail("Landscape stack limit");
        }
        switch (op) {
            case O::Save: {
                auto b = bitmap(true);
                if (!b || outputs_.size() >= 64) {
                    return fail("Landscape export failed");
                }
                outputs_.push_back({c.name, std::move(*b)});
                break;
            }
            case O::Get:
                if (!bitmap(i(0) != 0)) {
                    return fail("Landscape get failed");
                }
                break;
            case O::Swap:
                n.swap();
                break;
            case O::Dup:
                n.dup();
                break;
            case O::Size:
                if (!dimension(c.arguments[0])) {
                    return fail("Invalid landscape size");
                }
                n.size(i(0));
                break;
            case O::Add:
                n.add(a(0));
                break;
            case O::Sub:
                n.sub(a(0));
                break;
            case O::Mul:
                n.mul(a(0));
                break;
            case O::Div:
                if (a(0) == 0) {
                    return fail("Landscape divide by zero");
                }
                n.div(a(0));
                break;
            case O::Exp:
                n.exp(a(0));
                break;
            case O::Neg:
                n.neg();
                break;
            case O::Clr:
                n.clr(a(0));
                break;
            case O::Diff:
                n.diff();
                break;
            case O::Normalize:
                n.normalize(a(0), a(1));
                break;
            // execV ignores the script's stack index for ADD_STACK/SUB_STACK.
            case O::AddStack:
                n.addStack();
                break;
            case O::SubStack:
                n.subStack();
                break;
            case O::Overlay:
            case O::Blend:
                if (i(0) < 1 || std::size_t(i(0)) >= s.size() ||
                    s[0]->size != s[std::size_t(i(0))]->size) {
                    return fail("Invalid landscape source matrix");
                }
                if (op == O::Overlay) {
                    n.overlay(i(0), a(1));
                } else {
                    if (i(1) < 0 || i(1) > 16) {
                        return fail("Landscape blend distance limit");
                    }
                    n.blend(i(0), i(1));
                }
                break;
            case O::Rot:
                n.rot();
                break;
            case O::FlipX:
                n.flipX();
                break;
            case O::FlipY:
                n.flipY();
                break;
            case O::Fft:
                if (i(0) != 1 && i(0) != -1) {
                    return fail("Invalid FFT direction");
                }
                n.fft(i(0));
                break;
            case O::FillNormal:
                n.fillNormal(a(0));
                break;
            case O::Smooth:
                if (a(0) < 0 || a(1) < 0) {
                    return fail("Invalid smooth parameters");
                }
                n.smooth(a(0), a(1));
                break;
            case O::Slope:
            case O::Curve:
                if (i(1) < 0 || i(1) > 32 || a(0) < 0) {
                    return fail("Landscape filter work limit");
                }
                if (op == O::Slope) {
                    n.slope(a(0), i(1));
                } else {
                    n.curve(a(0), i(1));
                }
                break;
            case O::Shave:
                if (a(0) < 0 || a(1) < 0 || a(2) < 0 || a(2) > 1) {
                    return fail("Invalid shave parameters");
                }
                n.shaveArea(a(0), a(1), a(2));
                break;
            case O::Floor:
            case O::Ceil:
                if (a(1) < 0 || a(2) < 0) {
                    return fail("Invalid floor/ceil parameters");
                }
                if (op == O::Floor) {
                    n.floor(a(0), a(1), a(2));
                } else {
                    n.ceil(a(0), a(1), a(2));
                }
                break;
            case O::AlphaBlend:
                if (s.size() < 3 || s[0]->size != s[1]->size) {
                    return fail("Invalid alpha matrices");
                }
                n.alphablend(i(0), i(1));
                break;
            case O::Tile:
                if (s[0]->size > 256) {
                    return fail("Landscape tile size limit");
                }
                n.tile();
                break;
            case O::Wrap:
                n.wrap();
                break;
            case O::Clamp:
            case O::Mask:
                if (i(0) < 0 || i(0) > 9 || a(1) < 0) {
                    return fail("Invalid landscape detail");
                }
                if (op == O::Clamp) {
                    n.clamp(i(0), a(1));
                } else {
                    n.mask(i(0), a(1));
                }
                break;
            case O::ClipMin:
                n.clipMin(a(0));
                break;
            case O::ClipMax:
                n.clipMax(a(0));
                break;
            default:
                return fail("Unknown landscape opcode");
        }
    }
    if (!n.mExecStatus) {
        return fail(n.mExecString);
    }
    std::size_t total{};
    for (const auto& m : s) {
        total += m->values.size();
        if (total > 4'194'304 || !std::ranges::all_of(m->values, [](float f) {
                return std::isfinite(f) && std::abs(f) <= 1e9f;
            })) {
            return fail("Landscape result exceeds numeric/storage limits");
        }
    }
    return true;
}

bool Landscape::execute(const LandscapeCommand& c, const Loader& loader) {
    Landscape next(*this);
    next.error_.clear();
    if (!next.run(c, loader)) {
        error_ = next.error_;
        return false;
    }
    *this = std::move(next);
    return true;
}

bool Landscape::script(std::string_view script, bool clear, const Loader& loader) {
    if (script.size() > 1024U * 1024U) {
        error_ = "Landscape script size limit";
        return false;
    }
    std::vector<std::string> tokens;
    std::string token;
    for (std::size_t j = 0; j < script.size(); ++j) {
        const auto c = script[j];
        if (c == '/' && j + 1 < script.size() && script[j + 1] == '/') {
            while (j < script.size() && script[j] != '\n') {
                ++j;
            }
            if (!token.empty()) {
                tokens.push_back(std::exchange(token, {}));
            }
            continue;
        }
        if (c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n') {
            if (!token.empty()) {
                tokens.push_back(std::exchange(token, {}));
            }
        } else {
            token += c;
            if (token.size() > 255) {
                error_ = "Landscape token size limit";
                return false;
            }
        }
    }
    if (!token.empty()) {
        tokens.push_back(std::move(token));
    }
    Landscape next(*this);
    next.error_.clear();
    next.outputs_.clear();
    if (clear) {
        next.impl_->native.stack.clear();
    }
    std::size_t j{}, count{};
    while (j < tokens.size()) {
        const auto name = upper(tokens[j++]);
        const auto at = std::ranges::find(operations, name, &Operation::name);
        if (at == std::end(operations) || ++count > 4096) {
            error_ = "Unknown opcode or script work limit";
            return false;
        }
        LandscapeCommand c{at->code, {}, {}};
        for (auto type : at->types) {
            if (j >= tokens.size()) {
                error_ = "Missing landscape argument";
                return false;
            }
            const auto& t = tokens[j++];
            if (type == 's') {
                c.name = t;
            } else {
                double v{};
                const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
                if (ec != std::errc{} || end != t.data() + t.size()) {
                    error_ = "Invalid landscape number";
                    return false;
                }
                c.arguments.push_back(v);
            }
        }
        if (!next.run(c, loader)) {
            error_ = next.error_;
            return false;
        }
    }
    *this = std::move(next);
    return true;
}

bool Landscape::apply(TerrainDocument& d, UInt32 block, Point2I origin) const {
    if (block >= d.blocks.size() || impl_->native.stack.empty() ||
        std::abs(Int64(origin.x)) > 1'000'000 || std::abs(Int64(origin.y)) > 1'000'000) {
        return false;
    }
    auto next = d;
    auto& b = next.blocks[block];
    const auto& m = *impl_->native.stack[0];
    const auto mask = m.size - 1;
    for (UInt32 y = 0; y <= b.width; ++y) {
        for (UInt32 x = 0; x <= b.width; ++x) {
            b.heights[std::size_t(y) * (b.width + 1) + x] =
                m.real[((origin.y + Int32(y) - 1) & mask) * m.size +
                       ((origin.x + Int32(x)) & mask)];
        }
    }
    if (!next.rebuildHeightRanges() || !next.valid()) {
        return false;
    }
    d = std::move(next);
    return true;
}
} // namespace opent1::world
