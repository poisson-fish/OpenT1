#include <opent1/video/smacker.hpp>
#include <ranges>
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace opent1::video {
namespace {
void require(bool ok) {
    if (!ok) {
        throw std::runtime_error("invalid or over-budget Smacker data");
    }
}

struct Bytes {
    std::span<const std::byte> data;
    std::size_t at{};

    std::span<const std::byte> take(std::size_t n) {
        require(n <= data.size() - at);
        const auto s = data.subspan(at, n);
        at += n;
        return s;
    }

    UInt8 u8() {
        return std::to_integer<UInt8>(take(1)[0]);
    }

    UInt32 u32() {
        UInt32 v{};
        for (auto i : std::views::iota(unsigned{0}, unsigned{4})) {
            v |= UInt32(u8()) << (i * 8);
        }
        return v;
    }
};

struct Bits {
    std::span<const std::byte> data;
    std::size_t at{};

    UInt32 get(unsigned n = 1) {
        require(n <= 16 && n <= data.size() * 8 - at);
        UInt32 v{};
        for (const auto i : std::views::iota(0U, n)) {
            v |= UInt32((std::to_integer<unsigned>(data[at / 8]) >> (at % 8)) & 1) << i;
            ++at;
        }
        return v;
    }
};

struct Node {
    int left{-1}, right{-1};
    UInt16 value{};
    int cache{-1};
};

struct Tree {
    std::vector<Node> nodes;

    int small(Bits& b, unsigned depth = 0) {
        require(depth <= 32 && nodes.size() < 511);
        const int id = int(nodes.size());
        nodes.emplace_back();
        if (b.get()) {
            const int l = small(b, depth + 1), r = small(b, depth + 1);
            nodes[std::size_t(id)].left = l;
            nodes[std::size_t(id)].right = r;
        } else {
            nodes[std::size_t(id)].value = UInt16(b.get(8));
        }
        return id;
    }

    UInt16 symbol(Bits& b, std::array<UInt16, 3>* history = nullptr) const {
        std::size_t i{};
        while (nodes[i].left >= 0) {
            i = std::size_t(b.get() ? nodes[i].right : nodes[i].left);
        }
        const auto& n = nodes[i];
        const UInt16 value = n.cache >= 0 && history ? (*history)[std::size_t(n.cache)] : n.value;
        if (history && value != (*history)[0]) {
            (*history)[2] = (*history)[1];
            (*history)[1] = (*history)[0];
            (*history)[0] = value;
        }
        return value;
    }

    int big(Bits& b,
            const Tree& lo,
            const Tree& hi,
            const std::array<UInt16, 3>& escapes,
            std::size_t limit,
            unsigned depth = 0) {
        require(depth <= 64 && nodes.size() < limit);
        const int id = int(nodes.size());
        nodes.emplace_back();
        if (b.get()) {
            const int l = big(b, lo, hi, escapes, limit, depth + 1),
                      r = big(b, lo, hi, escapes, limit, depth + 1);
            nodes[std::size_t(id)].left = l;
            nodes[std::size_t(id)].right = r;
        } else {
            auto& n = nodes[std::size_t(id)];
            n.value = UInt16(lo.symbol(b) | (hi.symbol(b) << 8));
            for (auto j : std::views::iota(0, 3)) {
                if (n.value == escapes[std::size_t(j)]) {
                    n.cache = j;
                    break;
                }
            }
        }
        return id;
    }
};

Tree smallTree(Bits& b) {
    Tree t;
    if (b.get()) {
        t.small(b);
        require(b.get() == 0);
    } else {
        t.nodes.emplace_back();
    }
    return t;
}

Tree bigTree(Bits& b, UInt32 size) {
    Tree t;
    if (!b.get()) {
        t.nodes.emplace_back();
        return t;
    }
    const auto lo = smallTree(b), hi = smallTree(b);
    std::array<UInt16, 3> escapes{};
    for (auto& e : escapes) {
        e = UInt16(b.get(16));
    }
    require(size >= 4 && size <= 1024U * 1024);
    t.big(b, lo, hi, escapes, (size + 3U) / 4U);
    require(b.get() == 0);
    return t;
}

struct Chunk {
    std::size_t offset{}, size{};
    UInt32 decoded{};
    UInt64 first{};
};

struct Packet {
    std::size_t palette{}, paletteSize{}, video{}, videoSize{};
    std::array<Chunk, 7> audio;
};

Pcm pcm(std::span<const std::byte> bytes, const Chunk& c, AudioFormat format, bool packed) {
    Pcm out{format, c.first, {}};
    if (!c.size) {
        return out;
    }
    const auto input = bytes.subspan(c.offset, c.size);
    if (!packed) {
        out.bytes.assign(input.begin(), input.end());
        return out;
    }
    Bits b{input.subspan(4)};
    if (!b.get()) {
        require(!c.decoded);
        return out;
    }
    require(b.get() == UInt32(format.channels == 2) && b.get() == UInt32(format.bits == 16));
    const auto sampleBytes = UInt32(format.bits / 8), block = sampleBytes * format.channels;
    require(c.decoded >= block && c.decoded % block == 0);
    std::array<Tree, 4> trees;
    for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                      return i < block;
                  })) {
        require(b.get() == 1);
        trees[i].small(b);
        require(b.get() == 0);
    }
    std::array<UInt16, 2> predictor{};
    for (UInt32 i = format.channels; i > 0; --i) {
        // 16-bit initial predictors are stored high byte first in the LSB stream.
        const auto high = b.get(8);
        predictor[i - 1] = UInt16(sampleBytes == 2 ? (high << 8) | b.get(8) : high);
    }
    out.bytes.resize(c.decoded);
    for (UInt32 at = 0; at < c.decoded; at += block) {
        for (auto channel :
             std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 channel) {
                 return channel < format.channels;
             })) {
            if (at) {
                UInt32 delta = trees[channel * sampleBytes].symbol(b);
                if (sampleBytes == 2) {
                    delta |= UInt32(trees[channel * sampleBytes + 1].symbol(b)) << 8;
                }
                predictor[channel] = UInt16(predictor[channel] + delta);
            }
            const auto target = at + channel * sampleBytes;
            out.bytes[target] = std::byte(predictor[channel] & 255);
            if (sampleBytes == 2) {
                out.bytes[target + 1] = std::byte(predictor[channel] >> 8);
            }
        }
    }
    return out;
}

void palette(std::span<const std::byte> input, gfx::Palette& pal) {
    Bytes b{input};
    const auto old = pal.palettes[0].colors;
    auto& colors = pal.palettes[0].colors;
    UInt32 i{};
    while (i < 256) {
        const auto op = b.u8();
        if (op & 128) {
            const UInt32 count = (op & 127) + 1;
            require(count <= 256 - i);
            i += count;
        } else if (op & 64) {
            const auto from = b.u8();
            const UInt32 count = (op & 63) + 1;
            require(count <= 256 - i && count <= 256U - from);
            for (auto j : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 j) {
                              return j < count;
                          })) {
                colors[i++] = old[from + j];
            }
        } else {
            const auto g = b.u8(), bl = b.u8();
            require(g < 64 && bl < 64);
            const auto expand = [](UInt8 v) {
                return UInt8((v << 2) | (v >> 4));
            };
            colors[i++] = {expand(op), expand(g), expand(bl), 0};
        }
    }
}
}

struct Movie::Data {
    std::vector<std::byte> bytes;
    UInt32 width{}, height{}, frames{}, version{}, flags{};
    UInt64 time{};
    std::array<AudioFormat, 7> formats{};
    std::array<bool, 7> packed{};
    std::array<UInt64, 7> samples{};
    std::array<Tree, 4> trees;
    std::vector<Packet> packets;
};

Movie::Movie(std::shared_ptr<const Data> d) : data_(std::move(d)) {}

std::unique_ptr<Movie> Movie::load(StreamIO& stream, UInt32 size) {
    try {
        require(size >= 104 && size <= MaxFileBytes);
        auto d = std::make_shared<Data>();
        d->bytes.resize(size);
        require(stream.readBytes(d->bytes));
        Bytes b{d->bytes};
        const auto magic = b.u32();
        require(magic == 0x324b4d53 || magic == 0x344b4d53);
        d->version = magic >> 24;
        d->width = b.u32();
        d->height = b.u32();
        d->frames = b.u32();
        const auto delay = std::bit_cast<Int32>(b.u32());
        d->flags = b.u32();
        require(d->width && d->height && d->width <= 4096 && d->height <= 4096 && !(d->width % 4) &&
                !(d->height % 4));
        require(d->frames && d->frames <= MaxFrames && !(d->flags & ~7U));
        d->time = delay < 0 ? UInt64(-Int64(delay)) * 10 : (delay ? UInt64(delay) * 1000 : 100000);
        require(d->time <= 86400000000ULL / d->frames);
        b.take(28);
        const auto treeBytes = b.u32();
        require(treeBytes && treeBytes <= 4U * 1024 * 1024);
        std::array<UInt32, 4> treeSizes{};
        for (auto& s : treeSizes) {
            s = b.u32();
        }
        for (auto t : std::views::iota(UInt32{0}, UInt32{7})) {
            const auto f = b.u32();
            const auto rate = f & 0xffffff;
            require(!(f & 0x0c000000));
            if (rate) {
                require(rate >= 1000 && rate <= 192000);
                d->formats[t] = {
                    rate, UInt16((f & 0x10000000) ? 2 : 1), UInt16((f & 0x20000000) ? 16 : 8)};
                d->packed[t] = (f & 0x80000000) != 0;
            } else {
                require(!(f & 0xf0000000));
            }
        }
        b.u32();
        const UInt32 count = d->frames + (d->flags & 1);
        std::vector<UInt32> sizes(count);
        for (auto& s : sizes) {
            s = b.u32() & ~3U;
            require(s > 0);
        }
        const auto types = b.take(count);
        Bits treeBits{b.take(treeBytes)};
        bool anyTree{};
        for (auto i : std::views::iota(UInt32{0}, UInt32{4})) {
            auto probe = treeBits;
            anyTree = probe.get() != 0 || anyTree;
            d->trees[i] = bigTree(treeBits, treeSizes[i]);
        }
        require(anyTree);
        d->packets.reserve(count);
        for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                          return i < count;
                      })) {
            const auto start = b.at;
            Bytes packet{b.take(sizes[i])};
            Packet p;
            const auto type = std::to_integer<UInt8>(types[i]);
            if (type & 1) {
                const auto n = UInt32(packet.u8()) * 4;
                require(n >= 4);
                p.palette = start + packet.at;
                p.paletteSize = n - 1;
                packet.take(n - 1);
            }
            for (auto t : std::views::iota(UInt32{0}, UInt32{7})) {
                if (type & (2U << t)) {
                    require(d->formats[t].rate != 0);
                    const auto n = packet.u32();
                    require(n >= 4);
                    auto& c = p.audio[t];
                    c.offset = start + packet.at;
                    c.size = n - 4;
                    packet.take(c.size);
                    require(c.size > 0 && c.size <= 8U * 1024 * 1024);
                    Bytes a{std::span<const std::byte>(d->bytes).subspan(c.offset, c.size)};
                    c.decoded = d->packed[t] ? a.u32() : UInt32(c.size);
                    require(c.decoded <= 8U * 1024 * 1024);
                    const UInt32 block = d->formats[t].channels * (d->formats[t].bits / 8);
                    require(c.decoded % block == 0);
                    c.first = d->samples[t];
                    if (i < d->frames) {
                        d->samples[t] += c.decoded / block;
                        require(d->samples[t] <= UInt64(d->formats[t].rate) * 86400);
                    }
                }
            }
            p.video = start + packet.at;
            p.videoSize = packet.data.size() - packet.at;
            require(p.videoSize > 0);
            d->packets.push_back(p);
        }
        require(b.at == b.data.size());
        return std::unique_ptr<Movie>(new Movie(std::move(d)));
    } catch (const std::exception&) {
        return {};
    }
}

UInt32 Movie::width() const {
    return data_->width;
}

UInt32 Movie::height() const {
    return data_->height;
}

UInt32 Movie::frameCount() const {
    return data_->frames;
}

UInt64 Movie::frameTimeUs() const {
    return data_->time;
}

UInt64 Movie::durationUs() const {
    return data_->time * data_->frames;
}

UInt32 Movie::verticalScale() const {
    return (data_->flags & 6) ? 2 : 1;
}

bool Movie::ringFrame() const {
    return (data_->flags & 1) != 0;
}

AudioFormat Movie::audioFormat(UInt32 t) const {
    return t < 7 ? data_->formats[t] : AudioFormat{};
}

UInt64 Movie::audioSamples(UInt32 t) const {
    return t < 7 ? data_->samples[t] : 0;
}

Pcm Movie::audio(UInt32 t, UInt64 first, UInt32 count) const {
    require(t < 7 && data_->formats[t].rate && count <= data_->formats[t].rate);
    const auto& d = *data_;
    const auto f = d.formats[t];
    Pcm out{f, first, {}};
    if (first >= d.samples[t] || !count) {
        return out;
    }
    const auto end = std::min(first + count, d.samples[t]);
    const UInt32 block = f.channels * (f.bits / 8);
    out.bytes.reserve(std::size_t(end - first) * block);
    for (auto i : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
                      return i < d.frames;
                  })) {
        const auto& c = d.packets[i].audio[t];
        const auto tail = c.first + c.decoded / block;
        if (!c.size || tail <= first) {
            continue;
        }
        if (c.first >= end) {
            break;
        }
        const auto decoded = pcm(d.bytes, c, f, d.packed[t]);
        const auto lo = std::max(first, c.first) - c.first, hi = std::min(end, tail) - c.first;
        out.bytes.insert(out.bytes.end(),
                         decoded.bytes.begin() + std::ptrdiff_t(lo * block),
                         decoded.bytes.begin() + std::ptrdiff_t(hi * block));
    }
    require(out.bytes.size() == (end - first) * block);
    return out;
}

bool registerSmackerLoader(ResourceManager& r) {
    return r.registerLoader<Movie>(".smk", Movie::load);
}

Decoder::Decoder(std::shared_ptr<const Movie> m) : movie_(std::move(m)) {
    require(bool(movie_));
}

Decoder::Decoder(Resource<Movie> r)
    : Decoder(r ? std::shared_ptr<const Movie>(r.get(),
                                               [r](const Movie*) mutable {
                                                   r.reset();
                                               })
                : nullptr) {}

bool Decoder::decode(UInt32 index, std::shared_ptr<const Frame>& previous, bool audio) const {
    const auto& d = *movie_->data_;
    const auto& p = d.packets[index];
    auto f = std::make_shared<Frame>();
    f->index = index;
    f->timeUs = UInt64(index) * d.time;
    if (previous) {
        f->bitmap = previous->bitmap;
    } else {
        auto bitmap = gfx::Bitmap::create(Int32(d.width), Int32(d.height));
        require(bool(bitmap));
        f->bitmap = std::move(*bitmap);
        f->bitmap.palette.emplace();
    }
    const auto bytes = std::span<const std::byte>(d.bytes);
    if (p.paletteSize) {
        palette(bytes.subspan(p.palette, p.paletteSize), *f->bitmap.palette);
    }
    if (audio) {
        for (auto t : std::views::iota(UInt32{0}, UInt32{7})) {
            f->audio[t] = pcm(bytes, p.audio[t], d.formats[t], d.packed[t]);
        }
    }
    Bits bits{bytes.subspan(p.video, p.videoSize)};
    std::array<std::array<UInt16, 3>, 4> history{};
    const auto symbol = [&](UInt32 tree) {
        return d.trees[tree].symbol(bits, &history[tree]);
    };
    const UInt32 across = d.width / 4, total = across * (d.height / 4);
    UInt32 block{};
    while (block < total) {
        const auto type = symbol(3);
        const UInt32 code = (type >> 2) & 63, run = code < 59 ? code + 1 : 128U << (code - 59);
        UInt32 mode{};
        if ((type & 3) == 1 && d.version == '4') {
            mode = bits.get() ? 1U : (bits.get() ? 2U : 0U);
        }
        const auto endBlock = block + std::min(run, total - block);
        for (const auto currentBlock : std::views::iota(block, endBlock)) {
            if ((type & 3) == 2) {
                continue;
            }
            const UInt32 x = (currentBlock % across) * 4, y = (currentBlock / across) * 4;
            std::array<UInt8, 16> pixels{};
            if ((type & 3) == 3) {
                pixels.fill(UInt8(type >> 8));
            } else if ((type & 3) == 0) {
                const auto colors = symbol(1), map = symbol(0);
                for (auto k : std::views::iota(UInt32{0}, UInt32{16})) {
                    pixels[k] = UInt8((map & (1U << k)) ? colors >> 8 : colors & 255);
                }
            } else {
                const UInt32 rows = mode ? 2 : 4;
                for (auto row :
                     std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 row) {
                         return row < rows;
                     })) {
                    const auto right = symbol(2);
                    const auto left = mode == 1 ? right : symbol(2);
                    const std::array<UInt8, 4> line =
                        mode == 1
                            ? std::array<UInt8, 4>{UInt8(left),
                                                   UInt8(left),
                                                   UInt8(left >> 8),
                                                   UInt8(left >> 8)}
                            : std::array<UInt8, 4>{
                                  UInt8(left), UInt8(left >> 8), UInt8(right), UInt8(right >> 8)};
                    const UInt32 repeats = mode ? 2 : 1;
                    for (auto r :
                         std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 r) {
                             return r < repeats;
                         })) {
                        std::copy(
                            line.begin(), line.end(), pixels.begin() + (row * repeats + r) * 4);
                    }
                }
            }
            for (auto r : std::views::iota(UInt32{0}, UInt32{4})) {
                auto target = f->bitmap.row(Int32(y + r));
                for (auto col : std::views::iota(UInt32{0}, UInt32{4})) {
                    target[x + col] = std::byte(pixels[r * 4 + col]);
                }
            }
        }
        block = endBlock;
    }
    previous = std::move(f);
    return true;
}

bool Decoder::next() {
    const auto index = frame_ ? frame_->index + 1 : 0;
    if (index >= movie_->frameCount()) {
        return false;
    }
    try {
        auto candidate = frame_;
        decode(index, candidate);
        frame_ = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Decoder::seek(UInt32 index) {
    if (index >= movie_->frameCount()) {
        return false;
    }
    if (frame_ && frame_->index == index) {
        return true;
    }
    const UInt32 start = frame_ && frame_->index <= index ? frame_->index + 1 : 0;
    if (UInt64(index - start + 1) * (movie_->width() / 4) * (movie_->height() / 4) > 268435456ULL) {
        return false;
    }
    try {
        auto candidate = start ? frame_ : nullptr;
        for (auto i :
             std::views::iota(static_cast<UInt32>(start)) | std::views::take_while([&](UInt32 i) {
                 return i <= index;
             })) {
            decode(i, candidate, i == index);
        }
        frame_ = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace opent1::video
