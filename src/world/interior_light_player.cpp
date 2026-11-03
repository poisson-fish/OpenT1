#include <opent1/world/interior_light_player.hpp>
#include <ranges>

namespace opent1::world {
namespace {
constexpr std::size_t MaxWork = 4'194'304;

bool charge(std::size_t& work, std::size_t count = 1) {
    if (count > MaxWork - work) {
        return false;
    }
    work += count;
    return true;
}

std::array<UInt8, 4>
color(const InteriorLightState& a, const InteriorLightState& b, Flt32 multiplier) {
    std::array<UInt8, 4> result{};
    std::ranges::transform(
        std::views::iota(std::size_t{}, result.size()), result.begin(), [&](auto i) {
            return UInt8(std::clamp(
                (Flt32(a.color[i]) + (Flt32(b.color[i]) - a.color[i]) * multiplier) / 256 + .5f,
                0.f,
                255.f));
        });
    return result;
}
}

std::optional<InteriorLightPlayer> InteriorLightPlayer::create(const InteriorLighting& lighting,
                                                               UInt32 seed) {
    if (!lighting.validAnimation() || std::ranges::any_of(lighting.lights, [&](const auto& l) {
            return (l.flags & 4U) &&
                   (l.stateCount < 2 || lighting.states[std::size_t(l.firstState) + 1].time <= 0);
        })) {
        return {};
    }
    InteriorLightPlayer result(seed);
    result.lighting_ = std::make_shared<const InteriorLighting>(lighting);
    result.slots_.resize(std::size_t(lighting.mapSlots));
    result.threads_.resize(lighting.lights.size());
    result.installed_.resize(lighting.states.size());
    std::size_t work{};
    const auto initialized =
        std::ranges::all_of(std::views::iota(std::size_t{}, lighting.lights.size()), [&](auto i) {
            const auto& l = lighting.lights[i];
            const auto& s = lighting.states[std::size_t(l.firstState)];
            if (!charge(work, std::size_t(s.dataCount))) {
                return false;
            }
            auto& thread = result.threads_[i];
            thread.playback.time = s.time;
            thread.playback.playing = (l.flags & 1U) != 0;
            thread.color = color(s, s, 0);
            result.install(i);
            return true;
        });
    return initialized ? std::optional(std::move(result)) : std::nullopt;
}

std::optional<std::size_t> InteriorLightPlayer::find(Int32 id) const {
    const auto it = std::ranges::find(lighting_->lights, id, &InteriorLightAnimation::id);
    return it == lighting_->lights.end()
               ? std::nullopt
               : std::optional(std::size_t(it - lighting_->lights.begin()));
}

std::optional<InteriorLightPlayback> InteriorLightPlayer::playback(Int32 id) const {
    const auto i = find(id);
    return i ? std::optional(threads_[*i].playback) : std::nullopt;
}

void InteriorLightPlayer::install(std::size_t i) {
    const auto& thread = threads_[i];
    const auto& l = lighting_->lights[i];
    const auto& s = lighting_->states[std::size_t(l.firstState) + thread.playback.state];
    if (order_ == std::numeric_limits<UInt64>::max()) {
        std::vector<std::size_t> order;
        for (auto n : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t n) {
                          return n < installed_.size();
                      })) {
            if (installed_[n].order) {
                order.push_back(n);
            }
        }
        std::ranges::sort(order, [&](auto a, auto b) {
            return installed_[a].order < installed_[b].order;
        });
        order_ = 0;
        for (auto n : order) {
            installed_[n].order = ++order_;
        }
    }
    installed_[std::size_t(l.firstState) + thread.playback.state] = {++order_, thread.color};
    if (!s.dataCount) {
        return;
    }
    std::ranges::for_each(
        std::span(lighting_->stateData).subspan(std::size_t(s.dataIndex), std::size_t(s.dataCount)),
        [&](const auto& d) {
            auto& slot = slots_[std::size_t(d.slot)];
            slot.map = d.map;
            if (d.map >= 0) {
                slot.color = thread.color;
            }
        });
}

std::optional<std::vector<InteriorLightSlot>>
InteriorLightPlayer::slotsFor(const InteriorLighting& target) const {
    if (!target.validAnimation() || target.lights.size() != lighting_->lights.size()) {
        return {};
    }
    std::vector<Int32> mapped(lighting_->states.size(), -1);
    std::vector<bool> used(target.states.size());
    std::size_t work{};
    for (auto i : std::views::iota(std::size_t{0}, target.lights.size())) {
        const auto& a = lighting_->lights[i];
        const auto& b = target.lights[i];
        // Native ITRInstance preserves instance lights by ordinal when changing
        // detail. Authored IDs can change between LODs (e.g. acommand.dis).
        if (a.stateCount != b.stateCount || a.flags != b.flags || a.duration != b.duration ||
            !charge(work, std::size_t(a.stateCount))) {
            return {};
        }
        for (auto j : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 j) {
                          return j < a.stateCount;
                      })) {
            const auto from = std::size_t(a.firstState + j), to = std::size_t(b.firstState + j);
            const auto& sa = lighting_->states[from];
            const auto& sb = target.states[to];
            if (mapped[from] != -1 || used[to] || sa.time != sb.time || sa.color != sb.color) {
                return {};
            }
            mapped[from] = Int32(to);
            used[to] = true;
        }
    }
    std::vector<std::size_t> order;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < installed_.size();
                  })) {
        if (installed_[i].order) {
            order.push_back(i);
        }
    }
    std::ranges::sort(order, [&](auto a, auto b) {
        return installed_[a].order < installed_[b].order;
    });
    std::vector<InteriorLightSlot> result(std::size_t(target.mapSlots));
    for (auto i : order) {
        if (mapped[i] < 0) {
            return {};
        }
        const auto& s = target.states[std::size_t(mapped[i])];
        if (!charge(work, std::size_t(s.dataCount))) {
            return {};
        }
        if (!s.dataCount) {
            continue;
        }
        for (const auto& d : std::span(target.stateData)
                                 .subspan(std::size_t(s.dataIndex), std::size_t(s.dataCount))) {
            auto& slot = result[std::size_t(d.slot)];
            slot.map = d.map;
            if (d.map >= 0) {
                slot.color = installed_[i].color;
            }
        }
    }
    return result;
}

bool InteriorLightPlayer::update(std::size_t i, std::size_t& work) {
    auto& thread = threads_[i];
    auto& p = thread.playback;
    const auto& l = lighting_->lights[i];
    const auto states =
        std::span(lighting_->states).subspan(std::size_t(l.firstState), std::size_t(l.stateCount));
    UInt32 index{};
    Flt32 multiplier{};
    if (l.flags & 4U) {
        const auto unit = states[1].time;
        if (p.time < unit) {
            return true;
        }
        // The original consumes ONE random draw per update, even after skipping
        // multiple flicker intervals. Repeated subtraction preserves its clock.
        while (p.time >= unit) {
            if (!charge(work)) {
                return false;
            }
            p.time -= unit;
        }
        const auto value = random_.getFloat() * l.duration;
        while (index + 1 < states.size() && states[index + 1].time <= value) {
            if (!charge(work)) {
                return false;
            }
            ++index;
        }
        if (p.state == index) {
            return true;
        }
    } else {
        while (index + 1 < states.size() && p.time > states[index + 1].time) {
            if (!charge(work)) {
                return false;
            }
            ++index;
        }
        const auto end = index + 1 < states.size() ? states[index + 1].time : l.duration;
        multiplier = end != states[index].time
                         ? (p.time - states[index].time) / (end - states[index].time)
                         : 1;
        multiplier = std::clamp(multiplier, 0.f, 1.f);
    }
    const auto packed =
        color(states[index], states[index + 1 < states.size() ? index + 1 : 0], multiplier);
    if (p.state != index || packed != thread.color) {
        if (!charge(work, std::size_t(states[index].dataCount))) {
            return false;
        }
        p.state = index;
        thread.color = packed;
        install(i);
    }
    return true;
}

std::optional<std::vector<InteriorLightSlot>>
InteriorLightPlayer::slotsForDetail(const InteriorLighting& target) const {
    if (auto mapped = slotsFor(target)) {
        return mapped;
    }
    if (target.lights.size() < threads_.size()) {
        return {};
    }
    auto next = create(target);
    if (!next) {
        return {};
    }
    next->random_ = random_;
    std::size_t work{};
    for (auto i : std::views::iota(std::size_t{0}, threads_.size())) {
        next->threads_[i].playback = threads_[i].playback;
        auto& state = next->threads_[i].playback;
        if (state.time > target.lights[i].duration) {
            return {};
        }
        state.state = std::numeric_limits<UInt32>::max();
        if (!next->update(i, work)) {
            return {};
        }
    }
    return next->slots_;
}

bool InteriorLightPlayer::setTime(Int32 id, Flt32 seconds) {
    const auto i = find(id);
    if (!i || !std::isfinite(seconds) || seconds < 0 || seconds > lighting_->lights[*i].duration) {
        return false;
    }
    auto next = *this;
    auto& p = next.threads_[*i].playback;
    p.time = seconds;
    p.playing = true;
    std::size_t work{};
    if (!next.update(*i, work)) {
        return false;
    }
    *this = std::move(next);
    return true;
}

bool InteriorLightPlayer::setState(Int32 id, UInt32 state) {
    const auto i = find(id);
    if (!i || state >= UInt32(lighting_->lights[*i].stateCount)) {
        return false;
    }
    auto& thread = threads_[*i];
    thread.playback.playing = true;
    if (thread.playback.state != state) {
        const auto& s = lighting_->states[std::size_t(lighting_->lights[*i].firstState) + state];
        thread.playback.state = state;
        thread.playback.time = s.time;
        thread.color = color(s, s, 0);
        install(*i);
    }
    return true;
}

bool InteriorLightPlayer::setLoopCount(Int32 id, UInt32 loops) {
    const auto i = find(id);
    if (!i) {
        return false;
    }
    threads_[*i].playback.loops = loops;
    return true;
}

bool InteriorLightPlayer::setTimeScale(Int32 id, Flt32 scale) {
    const auto i = find(id);
    if (!i || !std::isfinite(scale) || scale < 0) {
        return false;
    }
    threads_[*i].playback.timeScale = scale;
    return true;
}

bool InteriorLightPlayer::setPlaying(Int32 id, bool playing) {
    const auto i = find(id);
    if (!i) {
        return false;
    }
    threads_[*i].playback.playing = playing;
    return true;
}

bool InteriorLightPlayer::advance(Flt32 seconds) {
    if (!std::isfinite(seconds) || seconds < 0) {
        return false;
    }
    auto next = *this;
    std::size_t work{};
    const auto advanced =
        std::ranges::all_of(std::views::iota(std::size_t{}, next.threads_.size()), [&](auto i) {
            auto& p = next.threads_[i].playback;
            const auto& l = lighting_->lights[i];
            if (!charge(work)) {
                return false;
            }
            if (!p.playing || p.timeScale == 0) {
                return true;
            }
            const auto delta = seconds * p.timeScale;
            if (!std::isfinite(delta) || !std::isfinite(p.time + delta)) {
                return false;
            }
            // Zero-duration authored lights are fixed states, never a subtract-zero loop.
            if (l.duration == 0) {
                p.playing = false;
                return true;
            }
            p.time += delta;
            while (p.time > l.duration) {
                if (!charge(work)) {
                    return false;
                }
                if (p.loops && --p.loops == 0) {
                    p.time =
                        (l.flags & 2U)
                            ? lighting_->states[std::size_t(l.firstState + l.stateCount - 1)].time
                            : 0;
                    p.playing = false;
                } else {
                    p.time -= l.duration;
                }
            }
            return next.update(i, work);
        });
    if (!advanced) {
        return false;
    }
    *this = std::move(next);
    return true;
}
} // namespace opent1::world
