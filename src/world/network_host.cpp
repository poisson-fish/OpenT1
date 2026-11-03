#include <opent1/world/network_host.hpp>
#include <ranges>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>

namespace opent1::world {
namespace {
template <class T> bool number(std::string_view text, T& result) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), result);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

struct Network {
    sim::SimGame& game;
    sim::SimManager* manager{};
    sim::SimAddress demo;
    bool owned{}, timed{};
    std::optional<UInt32> packetFrame;

    Network(sim::SimGame& g, std::optional<UInt32> frame) : game(g), packetFrame(frame) {}

    UInt32 now() const {
        const auto time = manager ? double(manager->getCurrentTime()) * 1000 : 0;
        return UInt32(std::fmod(time, 4294967296.));
    }

    sim::network::CSDelegate* delegate(sim::SimManager* target) const {
        return target
                   ? dynamic_cast<sim::network::CSDelegate*>(target->findObject(sim::CSDelegateId))
                   : nullptr;
    }

    std::vector<sim::SimAddress> streams(sim::SimManager& target) const {
        std::vector<sim::SimAddress> result;
        auto* set = dynamic_cast<sim::SimSet*>(target.findObject(sim::PacketStreamSetId));
        if (set) {
            for (auto* item : set->objects()) {
                if (dynamic_cast<sim::network::PacketStream*>(item)) {
                    result.push_back(item->address());
                }
            }
        }
        return result;
    }

    void stop() {
        const auto previous = demo;
        const bool destroy = owned;
        demo = {};
        owned = timed = false;
        if (manager) {
            if (auto* stream =
                    dynamic_cast<sim::network::PacketStream*>(previous.resolve(*manager))) {
                stream->endPlayback();
                if (destroy && manager) {
                    if (auto* current = previous.resolve(*manager)) {
                        manager->deleteObject(*current);
                    }
                }
            }
        }
    }

    void change(sim::SimManager* next) {
        if (next != manager) {
            stop();
            manager = next;
        }
    }

    std::string_view lookupClassName(Int32 tag) const {
        if (const auto* desc = game.classes().find(static_cast<UInt32>(tag))) {
            return desc->name;
        }
        switch (tag) {
            case 1:
                return "SimVolume";
            case 3:
                return "SimPalette";
            case 5:
                return "InteriorShape";
            case 6:
                return "SimLight";
            case 129:
                return "SimContainer";
            case 131:
                return "SimSoundSource";
            case 132:
                return "SimTerrain";
            case 133:
                return "SimSky";
            case 134:
                return "SimVolumetric";
            case 1026:
                return "SimMessageEvent";
            case 1032:
                return "SimConsoleEvent";
            case 1033:
            case 1059:
                return "SimSoundSequenceEvent";
            case 1044:
            case 1061:
                return "SimPathEvent";
            case 1057:
                return "Net::RemoteCreateEvent";
            default:
                return {};
        }
    }

    void syncRates(Console& console) {
        const auto frameText = console.getVariable("pref::PacketFrame");
        const auto rateText = console.getVariable("pref::PacketRate");
        const auto sizeText = console.getVariable("pref::PacketSize");
        if (!frameText.empty()) {
            Int32 pst = 32;
            if (!number(frameText, pst)) {
                pst = 32;
            }
            packetFrame = static_cast<UInt32>(std::clamp(pst, 32, 128));
        }
        if (rateText.empty() && sizeText.empty() && frameText.empty()) {
            return;
        }
        Int32 rate = 10, size = 200;
        if (!rateText.empty() && !number(rateText, rate)) {
            rate = 10;
        }
        if (!sizeText.empty() && !number(sizeText, size)) {
            size = 200;
        }
        const auto clampedRate = static_cast<UInt32>(std::clamp(rate, 1, 30));
        const auto clampedSize = static_cast<UInt32>(std::clamp(size, 100, 450));
        const auto apply = [&](sim::SimManager& target) {
            for (const auto& address : streams(target)) {
                if (auto* stream =
                        dynamic_cast<sim::network::PacketStream*>(address.resolve(target))) {
                    if (!frameText.empty()) {
                        (void)stream->setPacketFrame(packetFrame);
                    }
                    if (!rateText.empty() || !sizeText.empty()) {
                        (void)stream->setMaxRate(clampedRate, clampedSize);
                    }
                }
            }
        };
        if (manager) {
            apply(*manager);
        }
        for (const auto worldType : {sim::WorldType::Server, sim::WorldType::Client}) {
            if (auto* w = game.world(worldType); w && &w->manager() != manager) {
                apply(w->manager());
            }
        }
    }

    bool playback(std::string_view file, bool timeDemo) {
        if (!manager) {
            return false;
        }
        auto* del = delegate(manager);
        if (!del) {
            return false;
        }
        std::vector<std::byte> bytes;
        if (auto* resource = sim::SimResource::find(*manager);
            resource && resource->get().contains(file)) {
            if (!resource->get().read(file, bytes)) {
                return false;
            }
        } else {
            FileRStream input{std::filesystem::path(file)};
            if (input.getStatus() != STRM_OK || input.getSize() < 8 ||
                input.getSize() > 64 * 1024 * 1024) {
                return false;
            }
            bytes.resize(input.getSize());
            if (!input.readBytes(bytes)) {
                return false;
            }
        }
        if (bytes.size() < 8) {
            return false;
        }
        MemRWStream header{std::span<const std::byte>(bytes)};
        UInt32 owner{}, id{};
        if (!header.read(owner) || !header.read(id) || owner < 2048 || owner >= 3072 || id < 2048 ||
            id >= 3072) {
            return false;
        }
        if (demo.generation && demo.resolve(*manager)) {
            return false;
        }
        if (!streams(*manager).empty() && owner != manager->getId()) {
            return false;
        }
        if (!manager->setNetworkId(owner)) {
            return false;
        }
        auto* stream = dynamic_cast<sim::network::PacketStream*>(manager->findObject(id));
        bool created = false;
        if (!stream) {
            auto object =
                std::make_unique<sim::network::PacketStream>(del->registryShared(), false);
            object->setId(id);
            stream =
                static_cast<sim::network::PacketStream*>(manager->addObject(std::move(object)));
            if (!stream) {
                return false;
            }
            created = true;
            const auto add = [&](std::unique_ptr<sim::SimObject> client) {
                return manager->addObject(std::move(client), stream) != nullptr;
            };
            if (!add(std::make_unique<sim::network::EventManager>()) ||
                !add(std::make_unique<sim::network::GhostManager>())) {
                manager->deleteObject(*stream);
                return false;
            }
            try {
                if (del->configureStream) {
                    del->configureStream(*stream);
                }
            } catch (...) {
                manager->deleteObject(*stream);
                return false;
            }
        }
        if (!stream->startPlayback(
                bytes, sim::network::PacketStream::DemoTiming::Timestamped, now())) {
            if (created) {
                manager->deleteObject(*stream);
            }
            return false;
        }
        demo = stream->address();
        owned = created;
        timed = timeDemo;
        game.console().setVariable("playingDemo", "true");
        if (del->onConnection) {
            net::Event accepted;
            accepted.type = net::EventType::Accepted;
            del->onConnection(accepted);
        }
        return true;
    }

    std::optional<std::string> command(Console& console,
                                       sim::SimManager* current,
                                       std::string_view name,
                                       std::span<const std::string> args) {
        const auto yes = [](bool value) -> std::optional<std::string> {
            return value ? "True" : "False";
        };
        if (!manager || manager != current) {
            return "False";
        }
        syncRates(console);
        if (name == "netStats") {
            for (const auto& address : streams(*manager)) {
                if (!manager) {
                    return "False";
                }
                if (auto* stream =
                        dynamic_cast<sim::network::PacketStream*>(address.resolve(*manager))) {
                    console.print(
                        std::format("{}: {} queued events, {} byte packets, {} ms updates",
                                    stream->getId(),
                                    stream->eventManager() ? stream->eventManager()->queued() : 0,
                                    stream->rate().packetSize,
                                    stream->rate().updateDelay));
                }
            }
            return "True";
        }
        if (name == "logPacketStats") {
            struct BitSortInfo {
                Int32 type{}, count{};
            };

            auto& ps = sim::network::PacketStream::globalStats();
            for (auto dir : std::views::iota(std::size_t{0}, std::size_t{2})) {
                std::vector<BitSortInfo> bsi;
                for (auto classCt : std::views::iota(Int32{0}, Int32{2048})) {
                    const auto bits = ps.bitAccumulator[dir][static_cast<std::size_t>(classCt)];
                    if (bits != 0) {
                        bsi.push_back({classCt, bits});
                        ps.bitAccumulator[dir][static_cast<std::size_t>(classCt)] = 0;
                    }
                }
                std::ranges::stable_sort(bsi, [](const auto& a, const auto& b) {
                    return a.count < b.count;
                });
                console.print(std::format("{} stats: {} packets",
                                          dir ? "Receive" : "Send",
                                          dir ? ps.totalRecv : ps.totalSent));
                for (const auto& entry : bsi) {
                    const auto className = lookupClassName(entry.type);
                    if (!className.empty()) {
                        console.print(std::format("{} {}", entry.count, className));
                    }
                }
            }
            ps.totalSent = 0;
            ps.totalRecv = 0;
            return "True";
        }
        if (name == "net::kick") {
            auto* world = game.world(sim::WorldType::Server);
            auto* server = world ? delegate(&world->manager()) : nullptr;
            UInt32 peer{};
            if (!server || !number(args[0], peer)) {
                return "False";
            }
            const auto reason =
                args.size() == 2 ? std::as_bytes(std::span(args[1])) : std::span<const std::byte>{};
            return yes(server->disconnect(peer, now(), reason));
        }
        auto* del = delegate(manager);
        if (!del) {
            return "False";
        }
        if (name == "DNet::TranslateAddress") {
            UInt16 port{};
            if (!number(args[1], port)) {
                return "";
            }
            return del->translateAddress(args[0], port);
        }
        if (name == "connect") {
            return yes(del->connect(args[0], now()) == net::Result::Ok);
        }
        if (name == "disconnect") {
            bool result = true;
            for (const auto& address : streams(*manager)) {
                if (del->stream(address.objectId)) {
                    result = del->disconnect(address.objectId, now()) && result;
                }
            }
            stop();
            return yes(result);
        }
        if (name == "playDemo" || name == "timeDemo") {
            return yes(playback(args[0], name == "timeDemo"));
        }
        return "False";
    }

    void frame() {
        if (!manager) {
            return;
        }
        syncRates(game.console());
        if (auto* del = delegate(manager)) {
            (void)del->process(now());
        }
        if (!manager) {
            return;
        }
        const auto peers = streams(*manager);
        for (const auto& address : peers) {
            if (!manager) {
                return;
            }
            if (auto* stream =
                    dynamic_cast<sim::network::PacketStream*>(address.resolve(*manager))) {
                (void)stream->setPacketFrame(packetFrame);
                (void)stream->checkPacketSend(now());
            }
        }
        if (!manager) {
            return;
        }
        if (auto* stream = dynamic_cast<sim::network::PacketStream*>(demo.resolve(*manager))) {
            if (stream->demoState() == sim::network::PacketStream::DemoState::Playback) {
                if (timed) {
                    (void)stream->stepReplay();
                } else {
                    (void)stream->processRecorder(now());
                }
            }
        }
        if (manager) {
            if (auto* stream = dynamic_cast<sim::network::PacketStream*>(demo.resolve(*manager));
                stream && stream->demoState() != sim::network::PacketStream::DemoState::Playback) {
                stop();
            }
        }
    }
};
}

ObjectHost nativeNetworkHost(sim::SimGame& game, std::optional<UInt32> packetFrame) {
    if (packetFrame && (*packetFrame < 32 || *packetFrame > 128)) {
        throw std::invalid_argument("network packet frame");
    }
    auto state = std::make_shared<Network>(game, packetFrame);
    ObjectHost host;
    host.managerChanged = [state](auto* manager) {
        state->change(manager);
    };
    host.command = [state](Console& console, auto* manager, auto name, auto args) {
        return state->command(console, manager, name, args);
    };
    host.frame = [state] {
        state->frame();
    };
    host.stop = [state] {
        state->change(nullptr);
    };
    return host;
}
}
