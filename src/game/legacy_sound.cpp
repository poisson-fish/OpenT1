#include <opent1/game/legacy_sound.hpp>
#include <algorithm>
#include <cmath>

namespace opent1::game {
void LegacySound::install(Console& console) {
    for (const auto name : {"newSfx",
                            "sfxSetFormat",
                            "sfxAdd2DProfile",
                            "sfxAdd3DProfile",
                            "sfxAddPair",
                            "sfxPlay",
                            "sfxStop",
                            "sfxSetMaxBuffers",
                            "sfxGetMaxBuffers"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console& con, Int32, std::span<const std::string> args) -> std::string {
                const std::string_view command(name);
                const auto id = [&](std::size_t i) {
                    return UInt32(script::toInteger(args[i]));
                };
                const auto number = [&](std::size_t i) {
                    return float(script::toNumber(args[i]));
                };
                if (command == "newSfx") {
                    if (args.size() != 1) {
                        return "False";
                    }
                    initialized_ = true;
                    return "True";
                }
                if (!initialized_) {
                    return "False";
                }
                if (command == "sfxSetMaxBuffers") {
                    if (args.size() != 2 || script::toInteger(args[1]) < 0)
                        return "False";
                    const auto count = UInt32(std::min<Int64>(script::toInteger(args[1]), 32));
                    if (setMaxBuffers && !setMaxBuffers(count))
                        return "False";
                    maxBuffers_ = count;
                    return "True";
                }
                if (command == "sfxGetMaxBuffers") {
                    if (args.size() != 1)
                        return "False";
                    con.print("Sfx::Manager::maxBuffers = " + std::to_string(maxBuffers_));
                    return std::to_string(maxBuffers_);
                }
                if (command == "sfxSetFormat") {
                    if (args.size() != 4) {
                        return "False";
                    }
                    const auto rate = script::toInteger(args[1]), bits = script::toInteger(args[2]);
                    if (rate < 1000 || rate > 192000 || (bits != 8 && bits != 16)) {
                        return "False";
                    }
                    // The modern backend converts source PCM to its device format.
                    // Retain the requested legacy format for preference queries.
                    con.setVariable("Sfx::Rate", args[1]);
                    con.setVariable("Sfx::Bits", args[2]);
                    con.setVariable("Sfx::Stereo", args[3]);
                    return "True";
                }
                if (command == "sfxAdd2DProfile" || command == "sfxAdd3DProfile") {
                    const bool spatial = command == "sfxAdd3DProfile";
                    if (args.size() < (spatial ? 5U : 3U) || id(1) == 0) {
                        return "False";
                    }
                    audio::Profile profile;
                    profile.baseVolume = number(2);
                    profile.minDistance = spatial ? number(3) : 30;
                    profile.maxDistance = spatial ? number(4) : 300;
                    profile.coneInsideAngle = 90;
                    profile.coneOutsideAngle = 180;
                    profile.coneVector = {0, -1, 0};
                    profile.flags = spatial ? UInt32(audio::Hardware3D) : 0;
                    std::size_t flags = spatial ? 5 : 3;
                    if (spatial && args.size() >= 11) {
                        profile.coneOutsideVolume = number(5);
                        profile.coneInsideAngle = id(6);
                        profile.coneOutsideAngle = id(7);
                        profile.coneVector = {number(8), number(9), number(10)};
                        flags = 11;
                    }
                    for (const auto& flag : args.subspan(flags)) {
                        const auto token = con.internString(flag);
                        const auto equal = [&](std::string_view expected) {
                            return con.internString(expected) == token;
                        };
                        if (equal("SFX_IS_LOOPING")) {
                            profile.flags |= audio::Loop;
                        } else if (equal("SFX_IS_STREAMING")) {
                            profile.flags |= audio::Stream;
                        } else if (equal("SFX_IS_PITCHING")) {
                            profile.flags |= audio::Pitch;
                        } else if (equal("SFX_IS_SOFTWARE_3D")) {
                            profile.flags =
                                (profile.flags & ~UInt32(audio::Hardware3D)) | audio::Software3D;
                        } else if (equal("SFX_IS_HARDWARE_3D")) {
                            profile.flags =
                                (profile.flags & ~UInt32(audio::Software3D)) | audio::Hardware3D;
                        }
                    }
                    if (!std::isfinite(profile.baseVolume) || !std::isfinite(profile.minDistance) ||
                        !std::isfinite(profile.maxDistance) || profile.minDistance < 0 ||
                        profile.maxDistance < profile.minDistance) {
                        return "False";
                    }
                    profiles_.insert_or_assign(id(1), profile);
                    return "True";
                }
                if (command == "sfxAddPair") {
                    if ((args.size() != 4 && args.size() != 5) || id(1) == 0) {
                        return "False";
                    }
                    const auto priority = args.size() == 5 ? number(4) : 0;
                    if (!std::isfinite(priority) || priority < 0 || priority > 1) {
                        return "False";
                    }
                    pairs_.insert_or_assign(id(1), Pair{id(2), args[3], priority});
                    return "True";
                }
                if (command == "sfxPlay") {
                    if (args.size() != 2 || !play) {
                        return "False";
                    }
                    const auto pair = pairs_.find(id(1));
                    if (pair == pairs_.end()) {
                        return "False";
                    }
                    const auto profile = profiles_.find(pair->second.profile);
                    if (profile == profiles_.end()) {
                        return "False";
                    }
                    const auto handle =
                        play(id(1), pair->second.file, profile->second, pair->second.priority);
                    con.print("sfxPlay handle = " + std::to_string(handle));
                    return handle ? std::to_string(handle) : "False";
                }
                if (command == "sfxStop") {
                    return args.size() == 2 && stop && stop(id(1)) ? "True" : "False";
                }
                return "False";
            });
    }
}
} // namespace opent1::game
