#include <opent1/gui/text_format.hpp>
#include <ranges>
#include <charconv>
#include <cctype>

namespace opent1::gui {
namespace {
Int32 number(std::string_view text, bool& valid) {
    // Native atoi accepts leading whitespace/sign and stops at trailing text.
    // Refuse overflow rather than depending on the original CRT's overflow.
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
        if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
            return 0;
        }
    }
    Int32 n{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), n);
    if (result.ec == std::errc::result_out_of_range) {
        valid = false;
        return 0;
    }
    return result.ec == std::errc{} ? n : 0;
}

char lower(char c) {
    return char(std::tolower(static_cast<unsigned char>(c)));
}
}

bool TextFormat::format(std::string_view source, Int32 width) {
    if (source.size() > 65536 || source.find('\0') != std::string_view::npos ||
        !fonts[0].resource || !fonts[0].resource->valid() || width < 1 || width > 32768) {
        return false;
    }
    Int32 minimum = 0;
    const auto scaled = [](Int64 n, Int32 scale) {
        const auto product = n * scale;
        return product / 65536 - (product < 0 && product % 65536 != 0 ? 1 : 0);
    };
    for (const auto& f : fonts) {
        if (f.resource) {
            const auto& info = f.resource->info;
            if (!f.resource->valid() || info.height < 1 || info.height > 32768 ||
                info.spacing < 0 || info.scale.x < 1 || info.scale.y < 1 ||
                info.scale.x > 64 * 65536 || info.scale.y > 64 * 65536) {
                return false;
            }
            minimum =
                std::max(minimum, Int32(scaled(f.resource->glyph('W').info.width, info.scale.x)));
        }
    }
    if (width < minimum) {
        return false;
    }
    std::string text(source);
    std::vector<Run> runs;
    std::vector<Image> images;
    Int32 minWidth = 0;
    UInt8 font = 0, justification = 0;
    Point2I offset;
    Int32 spacing = 2, hardLeft = 0, hardRight = width - 1, left = 0, right = width - 1,
          leftBottom = -1, rightBottom = -1;
    const auto space = Int32(scaled(fonts[0].resource->glyph(' ').info.width,
                                    fonts[0].resource->info.scale.x)),
               tab = space * 3;
    std::array<std::vector<Int64>, 10> advances;
    const auto measure = [&](std::string_view value) {
        auto& prefix = advances[font];
        const auto& f = *fonts[font].resource;
        if (prefix.empty()) {
            prefix.reserve(text.size() + 1);
            prefix.push_back(0);
            for (const auto c : text) {
                prefix.push_back(prefix.back() +
                                 ((f.info.flags & gfx::FontFixed) ? f.info.width
                                                                  : f.glyph(UInt8(c)).info.width) +
                                 f.info.spacing);
            }
        }
        const auto start = std::size_t(value.data() - text.data());
        return scaled(prefix[start + value.size()] - prefix[start] - f.info.spacing,
                      f.info.scale.x);
    };
    const auto newline = [&]() -> bool {
        const auto step =
            Int64(spacing) *
            scaled(fonts[font].resource->info.height, fonts[font].resource->info.scale.y) / 2;
        if (step < 1 || Int64(offset.y) + step > MaxCoordinate) {
            return false;
        }
        offset.y += Int32(step);
        if (leftBottom > 0 && offset.y >= leftBottom) {
            leftBottom = -1;
            left = hardLeft;
        }
        if (rightBottom > 0 && offset.y >= rightBottom) {
            rightBottom = -1;
            right = hardRight;
        }
        offset.x = left;
        return true;
    };
    std::size_t pos = 0, budget = 0, expansions = 0, lastClose = text.rfind('>');
    try {
        while (pos < text.size()) {
            if (++budget > 262144 || runs.size() > 65536) {
                return false;
            }
            if (text[pos] == '<' && lastClose != std::string::npos && pos < lastClose &&
                !(pos + 1 < text.size() && text[pos + 1] == '<')) {
                const auto end = text.find('>', pos);
                auto arg = text.substr(pos + 1, end - pos - 1);
                if (arg.size() > 255) {
                    return false;
                }
                const auto next = end + 1;
                if (arg.empty()) {
                    pos = next;
                    continue;
                }
                const auto kind = lower(arg[0]);
                const auto value = std::string_view(arg).substr(1);
                bool validNumber = true;
                const auto numeric = [&](std::string_view v) {
                    return number(v, validNumber);
                };
                if (kind == 'f') {
                    auto index = std::clamp(numeric(value), 0, 9);
                    font = UInt8(fonts[std::size_t(index)].resource ? index : 0);
                } else if (kind == 'j') {
                    const auto c = value.empty() ? 'l' : lower(value[0]);
                    justification = UInt8(c == 'c' ? 1 : c == 'r' ? 2 : 0);
                } else if (kind == 's') {
                    spacing = std::max(2, numeric(value));
                    if (spacing > 65536) {
                        return false;
                    }
                } else if (kind == 'n') {
                    if (offset.x > left && !newline()) {
                        return false;
                    }
                    while ((leftBottom > 0 && offset.y < leftBottom) ||
                           (rightBottom > 0 && offset.y < rightBottom)) {
                        if (!newline()) {
                            return false;
                        }
                    }
                    offset.x = left;
                } else if (kind == 'l' || kind == 'r') {
                    const auto count = std::max(0, numeric(value));
                    const auto margin = Int64(space) * count;
                    if (margin > 32768) {
                        return false;
                    }
                    if (kind == 'l') {
                        hardLeft = Int32(margin);
                        left = leftBottom > 0 && offset.y < leftBottom ? std::max(left, hardLeft)
                                                                       : hardLeft;
                        offset.x = std::max(offset.x, left);
                    } else {
                        hardRight = width - 1 - Int32(margin);
                        right = rightBottom > 0 && offset.y < rightBottom
                                    ? std::min(right, hardRight)
                                    : hardRight;
                        if (offset.x >
                                right - Int32(scaled(fonts[font].resource->glyph('W').info.width,
                                                     fonts[font].resource->info.scale.x)) &&
                            !newline()) {
                            return false;
                        }
                    }
                    if (right - left < minimum) {
                        return false;
                    }
                } else if (kind == 'v' && variable && value.size() <= 124) {
                    // Original signed-byte argument length expands only names <=124.
                    // Longer in-bounds V tags are consumed without invoking lookup.
                    if (++expansions > 256) {
                        return false;
                    }
                    auto replacement = variable(value);
                    if (!replacement || replacement->find('\0') != std::string::npos ||
                        replacement->size() > 65536 ||
                        text.size() - (next - pos) + replacement->size() > 65536) {
                        return false;
                    }
                    text.replace(pos, next - pos, *replacement);
                    for (auto& prefix : advances) {
                        prefix.clear();
                    }
                    lastClose = text.rfind('>');
                    continue;
                } else if (kind == 'b' && images.size() < 8 && bitmap) {
                    auto name = value;
                    Point2I padding{4, 0};
                    const auto colon = value.find(':');
                    if (colon != std::string_view::npos) {
                        const auto comma = value.find(',');
                        if (comma < colon) {
                            padding = {numeric(value.substr(0, comma)),
                                       numeric(value.substr(comma + 1, colon - comma - 1))};
                        }
                        name = value.substr(colon + 1);
                    }
                    if (padding.x < 0 || padding.y < 0 || padding.x > 32768 || padding.y > 32768) {
                        return false;
                    }
                    auto resource = bitmap(name);
                    if (resource) {
                        if (!resource->valid()) {
                            return false;
                        }
                        const auto w = resource->width + 2 * padding.x;
                        if (w <= width) {
                            while (w > right - offset.x + 1) {
                                if (leftBottom <= 0 && rightBottom <= 0 &&
                                    w > hardRight - hardLeft + 1) {
                                    return false;
                                }
                                if (!newline()) {
                                    return false;
                                }
                            }
                            Image image{resource, {}, justification, w};
                            const auto bottom = offset.y + resource->height + 2 * padding.y;
                            if (bottom > MaxCoordinate) {
                                return false;
                            }
                            if (justification == 0) {
                                image.offset = offset + padding;
                                offset.x += w;
                                minWidth = std::max(minWidth, offset.x);
                                leftBottom = std::max(leftBottom, bottom);
                                left = offset.x;
                            } else if (justification == 1) {
                                image.offset = Point2I{left, offset.y} + padding;
                                leftBottom = std::max(leftBottom, bottom);
                                left += w;
                                minWidth = std::max(minWidth, w);
                            } else {
                                rightBottom = std::max(rightBottom, bottom);
                                right -= w;
                                image.offset = Point2I{right, offset.y} + padding;
                            }
                            images.push_back(std::move(image));
                        }
                    }
                }
                if (!validNumber) {
                    return false;
                }
                pos = next;
                continue;
            }
            if (text[pos] == '\n') {
                if (!newline()) {
                    return false;
                }
                ++pos;
                continue;
            }
            if (text[pos] == '\t') {
                if (tab <= 0) {
                    return false;
                }
                offset.x = left + ((offset.x - left) / tab + 1) * tab;
                ++pos;
                continue;
            }
            // A doubled '<' emits the second bracket; a bracket without any closing
            // '>' is ordinary text in the native parser.
            const bool escaped = text[pos] == '<' && pos + 1 < text.size() &&
                                 text[pos + 1] == '<' && lastClose != std::string::npos &&
                                 pos < lastClose;
            if (escaped) {
                ++pos;
            }
            auto end = pos + 1;
            while (!escaped && end < text.size() && text[end] != '\n' && text[end] != '\t' &&
                   !(text[end] == '<' && lastClose != std::string::npos && end < lastClose)) {
                ++end;
            }
            if (right - offset.x + 1 < Int32(scaled(fonts[font].resource->glyph('W').info.width,
                                                    fonts[font].resource->info.scale.x)) &&
                !newline()) {
                return false;
            }
            while (pos < end) {
                const auto available = right - offset.x + 1;
                if (available < minimum) {
                    if (leftBottom > 0 || rightBottom > 0) {
                        if (!newline()) {
                            return false;
                        }
                        continue;
                    }
                    return false;
                }
                auto fit = pos, upper = end;
                while (fit < upper) {
                    const auto mid = fit + (upper - fit + 1) / 2;
                    if (measure(std::string_view(text).substr(pos, mid - pos)) <= available) {
                        fit = mid;
                    } else {
                        upper = mid - 1;
                    }
                }
                if (fit == pos) {
                    return false;
                }
                auto split = fit;
                if (fit < end) {
                    while (split > pos && text[split] != ' ') {
                        --split;
                    }
                    if (split == pos) {
                        if (offset.x >
                            left + Int32(scaled(fonts[font].resource->glyph('W').info.width,
                                                fonts[font].resource->info.scale.x))) {
                            if (text[pos] == ' ') {
                                ++pos;
                            }
                            if (!newline()) {
                                return false;
                            }
                            continue;
                        }
                        split = fit;
                    }
                }
                const auto count = split - pos;
                if (count == 0) {
                    return false;
                }
                runs.push_back({font,
                                justification,
                                offset,
                                justification == 2 ? right - offset.x : right - left,
                                UInt32(pos),
                                UInt32(count)});
                offset.x += Int32(measure(std::string_view(text).substr(pos, count)));
                minWidth = std::max(minWidth, offset.x);
                pos = split;
                if (pos < end) {
                    if (!newline()) {
                        return false;
                    }
                    while (pos < end && text[pos] == ' ') {
                        ++pos;
                    }
                }
            }
        }
    } catch (...) {
        return false;
    }
    if (offset.x > left && !newline()) {
        return false;
    }
    offset.y = std::max(offset.y, std::max(leftBottom, rightBottom));
    for (std::size_t i = 0; i < runs.size();) {
        if (!runs[i].justification) {
            ++i;
            continue;
        }
        const auto begin = i;
        Int32 total = 0;
        const auto y = runs[i].offset.y;
        const auto just = runs[i].justification;
        do {
            const auto& r = runs[i];
            total += Int32(fonts[r.font].resource->textWidth(
                std::string_view(text).substr(r.start, r.length)));
            ++i;
        } while (i < runs.size() && runs[i].offset.y == y && runs[i].justification == just);
        for (auto j : std::views::iota(begin) | std::views::take_while([&](auto j) {
                          return j < i;
                      })) {
            runs[j].offset.x += (runs[j].lineWidth - total) / (just == 1 ? 2 : 1);
        }
    }
    for (std::size_t i = 0; i < images.size();) {
        if (images[i].justification != 1) {
            ++i;
            continue;
        }
        const auto begin = i;
        Int32 total = 0;
        const auto y = images[i].offset.y;
        do {
            total += images[i++].width;
        } while (i < images.size() && images[i].offset.y == y && images[i].justification == 1);
        for (auto j : std::views::iota(begin) | std::views::take_while([&](auto j) {
                          return j < i;
                      })) {
            images[j].offset.x += (width - total) / 2;
        }
    }
    text_ = std::move(text);
    runs_ = std::move(runs);
    images_ = std::move(images);
    width_ = width;
    minWidth_ = minWidth;
    height_ = offset.y + 2;
    return true;
}

bool TextFormat::draw(gfx::SoftwareSurface& s, Point2I p, RectI) const {
    for (const auto& run : runs_) {
        if (run.font >= fonts.size() || !fonts[run.font].resource || run.start > text_.size() ||
            run.length > text_.size() - run.start ||
            !s.text(*fonts[run.font].resource,
                    p + run.offset,
                    std::string_view(text_).substr(run.start, run.length))) {
            return false;
        }
    }
    for (const auto& image : images_) {
        if (!image.bitmap || !s.blit(*image.bitmap, p + image.offset)) {
            return false;
        }
    }
    return true;
}

std::function<void()> TextFormat::checkpoint() {
    auto base = Active::checkpoint();
    auto t = text_;
    auto r = runs_;
    auto i = images_;
    const auto w = width_, m = minWidth_, h = height_;
    return [this,
            base = std::move(base),
            t = std::move(t),
            r = std::move(r),
            i = std::move(i),
            w,
            m,
            h] {
        base();
        text_ = t;
        runs_ = r;
        images_ = i;
        width_ = w;
        minWidth_ = m;
        height_ = h;
    };
}
} // namespace opent1::gui
