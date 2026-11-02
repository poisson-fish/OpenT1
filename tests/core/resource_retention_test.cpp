#include "test_support.hpp"
#include "test_registry.hpp"
#include <opent1/core/res_manager.hpp>
#include <opent1/core/vol_stream.hpp>
#include <fstream>

using namespace opent1;

namespace {
std::string value(const RawData& data) {
    return {reinterpret_cast<const char*>(data.bytes.data()), data.bytes.size()};
}

struct Dependent {
    std::shared_ptr<const RawData> data;
};

void archive(const std::filesystem::path& path,
             std::string_view name,
             std::string_view bytes,
             bool dependent = false) {
    VolumeRWStream volume;
    CHECK(volume.put(name, std::as_bytes(std::span(bytes))));
    if (dependent)
        CHECK(volume.put("thing.dep", std::as_bytes(std::span(bytes))));
    CHECK(volume.save(path));
}

int sourceRetention() {
    return test::run([] {
        const auto root = std::filesystem::absolute(
            "source-retention-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        archive(root / "first.vol", "asset.dat", "one", true);
        archive(root / "unrelated.vol", "other.dat", "other");
        archive(root / "override.vol", "asset.dat", "two");
        std::shared_ptr<const RawData> retained;
        {
            ResourceManager manager;
            CHECK(manager.setSourceOnlyLoader(".dat"));
            CHECK(!manager.setSourceOnlyLoader(".unknown"));
            CHECK(manager.registerLoader<Dependent>(".dep", [&](StreamIO&, UInt32) {
                auto result = std::make_unique<Dependent>();
                result->data = manager.load<RawData>("asset.dat").immutable();
                return result;
            }));
            const std::array paths{root};
            CHECK(manager.setSearchPaths(paths) && manager.addVolume("first.vol"));
            const auto one = manager.load<RawData>("asset.dat");
            const auto dependent = manager.load<Dependent>("thing.dep");
            CHECK(one && dependent && value(*one) == "one");
            const auto revision = one.revision();
            retained = one.immutable();
            CHECK(revision != 0);
            CHECK(manager.addVolume("unrelated.vol"));
            CHECK(manager.load<RawData>("asset.dat").get() == one.get());
            CHECK(manager.load<RawData>("asset.dat").revision() == revision);
            CHECK(manager.load<Dependent>("thing.dep").get() != dependent.get());
            CHECK(manager.setSearchPaths(paths));
            CHECK(manager.load<RawData>("asset.dat").revision() == revision);
            CHECK(manager.addVolume("override.vol"));
            const auto two = manager.load<RawData>("asset.dat");
            CHECK(two && value(*two) == "two" && two.revision() != revision &&
                  value(*one) == "one");
            const auto changedDependent = manager.load<Dependent>("thing.dep");
            CHECK(changedDependent && value(*changedDependent->data) == "two");
            CHECK(value(*dependent->data) == "one");
            CHECK(manager.removeVolume("override.vol"));
            CHECK(manager.load<RawData>("asset.dat").revision() == revision);
            {
                std::ofstream file(root / "asset.dat");
                file << "loose";
            }
            CHECK(manager.setSearchPaths(paths));
            const auto loose = manager.load<RawData>("asset.dat");
            CHECK(loose && value(*loose) == "loose" && loose.revision() != revision);
            {
                std::ofstream file(root / "asset.dat");
                file << "changed";
            }
            CHECK(manager.setSearchPaths(paths));
            CHECK(value(*manager.load<RawData>("asset.dat")) == "changed" &&
                  value(*loose) == "loose");
            CHECK(std::filesystem::remove(root / "asset.dat") && manager.setSearchPaths(paths));
            CHECK(manager.load<RawData>("asset.dat").revision() == revision);
            CHECK(manager.removeVolume("first.vol") && value(*retained) == "one");
        }
        CHECK(value(*retained) == "one");
        std::filesystem::remove_all(root);
    });
}
}

OPENT1_REGISTER_TEST("core", "source_resolution_retention", sourceRetention);
