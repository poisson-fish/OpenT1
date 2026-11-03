#include <opent1/world/interior_shape.hpp>
#include <ranges>
#include "asset_reader.hpp"
#include <set>
#include <map>

namespace opent1::world {
bool InteriorLighting::validAnimation() const {
    if (mapSlots < 0 || mapSlots > 65536 || surfaces.size() > 65536 || states.size() > 65536 ||
        stateData.size() > 65536 || lights.size() > 65536) {
        return false;
    }
    for (const auto& d : stateData) {
        if (d.surface < 0 || std::size_t(d.surface) >= surfaces.size() || d.slot < 0 ||
            d.slot >= mapSlots || d.map < -1) {
            return false;
        }
        const auto& s = surfaces[std::size_t(d.surface)];
        if (d.slot < s.lightIndex || Int32(d.slot) >= Int32(s.lightIndex) + s.lightCount ||
            (d.map >= 0 &&
             (std::size_t(d.map) > mapData.size() ||
              std::size_t(s.width) * s.height > mapData.size() - std::size_t(d.map)))) {
            return false;
        }
    }
    for (const auto& s : states) {
        if (!std::isfinite(s.time) || s.time < 0 || s.dataCount < 0 ||
            (s.dataCount &&
             (s.dataIndex < 0 || std::size_t(s.dataIndex) + s.dataCount > stateData.size())) ||
            std::ranges::any_of(
                s.color,
                [](auto c) {
                    return c > 0xff00U;
                })) {
            return false;
        }
    }
    std::set<Int32> ids;
    std::size_t work{};
    for (const auto& l : lights) {
        if (!ids.insert(l.id).second || l.stateCount < 1 || l.firstState < 0 ||
            std::size_t(l.firstState) + l.stateCount > states.size() ||
            !std::isfinite(l.duration) || l.duration < 0 ||
            (l.name != -1 && !asset_detail::name(names, UInt32(l.name)))) {
            return false;
        }
        work += std::size_t(l.stateCount);
        if (work > 4'194'304) {
            return false;
        }
        Flt32 previous{};
        for (const auto& s :
             std::span(states).subspan(std::size_t(l.firstState), std::size_t(l.stateCount))) {
            if (s.time < previous || s.time > l.duration) {
                return false;
            }
            previous = s.time;
        }
    }
    return true;
}

std::optional<std::vector<InteriorLightSlot>>
InteriorLighting::animation(Flt32 seconds, std::span<const InteriorLightControl> controls) const {
    if (!std::isfinite(seconds) || seconds < 0 || controls.size() > lights.size() ||
        !validAnimation()) {
        return {};
    }
    std::map<Int32, const InteriorLightControl*> controlled;
    for (const auto& c : controls) {
        if (!controlled.emplace(c.id, &c).second || !std::isfinite(c.time) || c.time < 0) {
            return {};
        }
    }
    std::vector<InteriorLightSlot> slots(static_cast<std::size_t>(mapSlots));
    std::size_t work{}, matched{};
    for (const auto& l : lights) {
        const auto found = controlled.find(l.id);
        const auto* control = found == controlled.end() ? nullptr : found->second;
        matched += control ? 1 : 0;
        UInt32 index{};
        Flt32 multiplier{};
        const bool explicitState = control && control->state.has_value();
        const bool timed = !explicitState && (control || ((l.flags & 1U) && !(l.flags & 4U)));
        if (explicitState) {
            index = *control->state;
            if (index >= UInt32(l.stateCount)) {
                return {};
            }
        } else if (timed) {
            if (l.flags & 4U) {
                return {};
            } // Random-frame selection needs an explicit state.
            const auto time =
                control ? control->time
                        : (l.duration > 0 ? Flt32(std::fmod(Flt64(seconds), l.duration)) : 0.f);
            if (time > l.duration) {
                return {};
            }
            while (index + 1 < UInt32(l.stateCount) &&
                   time > states[std::size_t(l.firstState) + index + 1].time) {
                ++index;
            }
            const auto& begin = states[std::size_t(l.firstState) + index];
            const auto endTime = index + 1 < UInt32(l.stateCount)
                                     ? states[std::size_t(l.firstState) + index + 1].time
                                     : l.duration;
            multiplier = endTime != begin.time ? (time - begin.time) / (endTime - begin.time) : 1;
            multiplier = std::clamp(multiplier, 0.f, 1.f);
        }
        const auto& state = states[std::size_t(l.firstState) + index];
        const auto& next =
            states[std::size_t(l.firstState) + (index + 1 < UInt32(l.stateCount) ? index + 1 : 0)];
        std::array<UInt8, 4> color{};
        for (auto i : std::views::iota(std::size_t{0}, std::size_t{4})) {
            const auto value =
                Flt32(state.color[i]) + (Flt32(next.color[i]) - state.color[i]) * multiplier;
            color[i] = UInt8(std::clamp(value / 256 + .5f, 0.f, 255.f));
        }
        if (!state.dataCount) {
            continue;
        }
        work += std::size_t(state.dataCount);
        if (work > 4'194'304) {
            return {};
        }
        for (const auto& d : std::span(stateData).subspan(std::size_t(state.dataIndex),
                                                          std::size_t(state.dataCount))) {
            slots[std::size_t(d.slot)] = {d.map, color};
        }
    }
    return matched == controls.size() ? std::optional(std::move(slots)) : std::nullopt;
}
} // namespace opent1::world
