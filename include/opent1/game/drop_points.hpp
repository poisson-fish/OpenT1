#pragma once

#include <opent1/core/stream_io.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/math/matrix.hpp>
#include <opent1/sim/sim_base.hpp>
#include <opent1/sim/sim_editor_events.hpp>
#include <opent1/sim/sim_resource_volume.hpp>

#include <string>
#include <string_view>

namespace opent1::game {

inline constexpr Int32 DropPointQueryType = 0x447;

struct DropPointQuery : public sim::SimQuery {
    std::string dropPointName{};
    TMat3F tmat{};

    explicit DropPointQuery(std::string_view name = {}) : dropPointName(name) {
        type = DropPointQueryType;
        tmat.identity();
    }

    ~DropPointQuery() override = default;
};

class DropPointGroup : public sim::SimGroup {
    using Parent = sim::SimGroup;

public:
    DropPointGroup();
    ~DropPointGroup() override;

    [[nodiscard]] bool isRandom() const noexcept {
        return random_;
    }

    void setRandom(bool r) noexcept {
        random_ = r;
    }

    [[nodiscard]] Int32 currentIndex() const noexcept {
        return currentIndex_;
    }

    void setCurrentIndex(Int32 idx) noexcept {
        currentIndex_ = idx;
    }

    void setDeterministicRngSeed(UInt32 seed) noexcept {
        useDeterministicRng_ = true;
        rngState_ = seed;
    }

    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

    bool inspectRead(sim::Inspect& inspector) override;
    bool inspectWrite(sim::Inspect& inspector) const override;

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

private:
    bool onDropPointQuery(DropPointQuery& query);
    Int32 nextRandIndex(Int32 count) noexcept;

    Int32 currentIndex_{-1};
    bool random_{false};
    bool useDeterministicRng_{false};
    UInt32 rngState_{1};
};

} // namespace opent1::game
