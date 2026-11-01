#pragma once
#include <opent1/audio/audio.hpp>
#include <opent1/console/console.hpp>

namespace opent1::game {
// Profiles can be defined before the host attaches an audio backend.
class LegacySound {
public:
    std::function<UInt64(UInt32, std::string_view, const audio::Profile&, float)> play;
    std::function<bool(UInt64)> stop;
    void install(Console&);

private:
    struct Pair {
        UInt32 profile{};
        std::string file;
        float priority{};
    };

    std::map<UInt32, audio::Profile> profiles_;
    std::map<UInt32, Pair> pairs_;
    bool initialized_{};
};
} // namespace opent1::game
