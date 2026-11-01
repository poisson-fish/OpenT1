#include <opent1/sim/sim_interior_grouping.hpp>
#include <opent1/sim/sim_editor_events.hpp>
#include <sstream>
#include <cmath>

namespace opent1::sim {
std::unique_ptr<InteriorGroupingScript> InteriorGroupingScript::load(StreamIO& stream,
                                                                     UInt32 size) {
    if (size > 1024U * 1024U) {
        return {};
    }
    std::string text(size, '\0');
    if (!stream.readBytes(std::as_writable_bytes(std::span(text))) ||
        text.find('\0') != text.npos) {
        return {};
    }
    std::istringstream input(text);
    std::string line;
    auto script = std::make_unique<InteriorGroupingScript>();
    while (std::getline(input, line)) {
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        if (line.size() > 4096 || script->lines.size() >= 65536) {
            return {};
        }
        script->lines.push_back(line);
    }
    if (script->lines.size() < 3 || script->lines.front() != "[EMF]" ||
        script->lines.back() != "[END]" || script->lines[1].empty()) {
        return {};
    }
    return script;
}

bool registerInteriorGroupingResource(ResourceManager& resources) {
    return resources.registerLoader<InteriorGroupingScript>(".emf", InteriorGroupingScript::load);
}

bool SimInteriorGrouping::processArguments(std::span<const std::string> args) {
    if (!game_ || !getManager() || args.size() != 1) {
        return false;
    }
    auto* resource = SimResource::find(*getManager());
    const auto script = resource ? resource->get().load<InteriorGroupingScript>(args[0])
                                 : Resource<InteriorGroupingScript>{};
    if (!script) {
        return false;
    }
    const auto& lines = script->lines;
    const auto resolve = [this](std::string_view path, bool parent) -> SimGroup* {
        SimGroup* group = this;
        while (!path.empty()) {
            const auto colon = path.find(':');
            if (colon == path.npos && parent) {
                break;
            }
            const auto part = path.substr(0, colon);
            if (part.empty() || part == "." || part == ".." ||
                part.find_first_of("/\\") != part.npos) {
                return nullptr;
            }
            group = dynamic_cast<SimGroup*>(group->findObject(part));
            if (!group) {
                return nullptr;
            }
            if (colon == path.npos) {
                break;
            }
            path.remove_prefix(colon + 1);
        }
        return group;
    };
    const std::array<std::string, 1> baseArgs{lines[1]};
    if (!game_->createObject("SimInterior", lines[1], this, baseArgs)) {
        return false;
    }
    for (std::size_t i = 2; i + 1 < lines.size(); ++i) {
        if (lines[i].starts_with("GROUP ")) {
            if (++i + 1 >= lines.size()) {
                return false;
            }
            const auto& path = lines[i];
            auto* parent = resolve(path, true);
            const auto colon = path.rfind(':');
            const auto name = path.substr(colon == path.npos ? 0 : colon + 1);
            const auto type = lines[i - 1].substr(6);
            if (!parent || name.empty() || !game_->createObject(type, name, parent)) {
                return false;
            }
            if (!dynamic_cast<SimGroup*>(parent->findObject(name))) {
                return false;
            }
        } else if (lines[i] == "OBJECT") {
            if (lines.size() - i < 8) {
                return false;
            }
            auto* parent = resolve(lines[++i], false);
            const auto& name = lines[++i];
            const auto& type = lines[++i];
            std::istringstream arguments(lines[++i]);
            std::vector<std::string> values;
            for (std::string value; arguments >> value;) {
                if (values.size() == 32) {
                    return false;
                }
                values.push_back(std::move(value));
            }
            Point3F position, rotation;
            const auto point = [](const std::string& line, Point3F& value) {
                std::istringstream input(line);
                return bool(input >> value.x >> value.y >> value.z) && std::isfinite(value.x) &&
                       std::isfinite(value.y) && std::isfinite(value.z);
            };
            if (!point(lines[++i], position) || !point(lines[++i], rotation) || !parent) {
                return false;
            }
            auto* object = game_->createObject(type, name, parent, values);
            if (!object) {
                return false;
            }
            SimObjectTransformEvent event;
            event.transform.set(EulerF{rotation.x, rotation.y, rotation.z}, position);
            object->processEvent(event);
        }
    }
    return true;
}
} // namespace opent1::sim
