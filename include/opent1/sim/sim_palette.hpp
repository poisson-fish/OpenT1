#pragma once
#include <opent1/gfx/palette.hpp>
#include <opent1/sim/sim_resource_volume.hpp>

namespace opent1::sim {
class SimWorld;

class SimPalette : public SimNetObject {
public:
    SimPalette() {
        setNetFlags(ScopeAlways | Ghostable);
    }

    bool onAdd() override;
    void onRemove() override;
    bool prepareArguments(std::span<const std::string>) override;
    bool processArguments(std::span<const std::string>) override;
    bool setFileName(std::string_view); // stages before registration, loads when live
    bool open(std::string_view);        // failed loads preserve the previous asset/name

    [[nodiscard]] std::string_view getFileName() const {
        return filename_;
    }

    [[nodiscard]] Resource<gfx::Palette> getPalette() const {
        return palette_;
    }

    bool setInWorld(SimWorld&, Int32 transitionColor = 0) const;
    bool packUpdate(BitStream&) const;
    bool unpackUpdate(BitStream&);

    UInt16 getGhostTag() const override {
        return 3;
    }

    UInt32 packUpdate(network::GhostManager&, UInt32, BitStream& b) const override {
        packUpdate(b);
        return 0;
    }

    bool unpackUpdate(network::GhostManager&, BitStream& b) override {
        return unpackUpdate(b);
    }

    bool inspectWrite(Inspect&) const override;
    bool inspectRead(Inspect&) override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    std::string filename_;
    Resource<gfx::Palette> palette_;
};

bool registerSimPaletteClass(Persistent::ClassRegistry&);
} // namespace opent1::sim
