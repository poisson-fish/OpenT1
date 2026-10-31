#include <opent1/game/drop_points.hpp>
#include <opent1/game/string_table.hpp>

#include <cstdlib>

namespace opent1::game {

DropPointGroup::DropPointGroup() = default;

DropPointGroup::~DropPointGroup() = default;

Int32 DropPointGroup::nextRandIndex(Int32 count) noexcept {
    if (count <= 0) {
        return 0;
    }
    if (useDeterministicRng_) {
        rngState_ = rngState_ * 214013U + 2531011U;
        const Int32 r = static_cast<Int32>((rngState_ >> 16U) & 0x7FFFU);
        return r % count;
    }
    return std::rand() % count;
}

bool DropPointGroup::onDropPointQuery(DropPointQuery& query) {
    sim::SimObject* obj = nullptr;

    if (!query.dropPointName.empty()) {
        obj = findObject(query.dropPointName);
        if (!obj) {
            if (auto* group = dynamic_cast<sim::SimGroup*>(findObject("NamedDropPoints"))) {
                obj = group->findObject(query.dropPointName);
            }
        }
    }

    const auto& list = objects();
    if (!obj && !list.empty()) {
        const Int32 count = static_cast<Int32>(list.size());
        Int32 attempt = 10;
        while (!obj && attempt-- > 0) {
            if (random_) {
                currentIndex_ = nextRandIndex(count);
            } else {
                if (++currentIndex_ >= count) {
                    currentIndex_ = 0;
                }
            }
            obj = list[static_cast<std::size_t>(currentIndex_)];
            if (obj && obj->getName() == "NamedDropPoints") {
                obj = nullptr;
            }
        }
    }

    if (obj) {
        sim::SimObjectTransformQuery queryTMat{};
        if (obj->processQuery(queryTMat)) {
            query.tmat = queryTMat.transform;
            return true;
        }
        if (auto* subGroup = dynamic_cast<sim::SimGroup*>(obj)) {
            const auto& subList = subGroup->objects();
            if (!subList.empty()) {
                const Int32 subCount = static_cast<Int32>(subList.size());
                const Int32 subIdx = random_ ? nextRandIndex(subCount) : 0;
                if (auto* child = subList[static_cast<std::size_t>(subIdx)]) {
                    if (child->processQuery(queryTMat)) {
                        query.tmat = queryTMat.transform;
                        return true;
                    }
                }
            }
        }
    }

    query.tmat.identity();
    return false;
}

bool DropPointGroup::processEvent(const sim::SimEvent& event) {
    return Parent::processEvent(event);
}

bool DropPointGroup::processQuery(sim::SimQuery& query) {
    if (auto* dpq = dynamic_cast<DropPointQuery*>(&query)) {
        return onDropPointQuery(*dpq);
    }
    return Parent::processQuery(query);
}

bool DropPointGroup::inspectWrite(sim::Inspect& inspector) const {
    if (!Parent::inspectWrite(inspector)) {
        return false;
    }
    inspector.write(IDITG_RANDOM, random_);
    return true;
}

bool DropPointGroup::inspectRead(sim::Inspect& inspector) {
    if (!Parent::inspectRead(inspector)) {
        return false;
    }
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Bool, IDITG_RANDOM, v)) {
        return false;
    }
    random_ = std::get<bool>(v);
    return true;
}

Persistent::Error
DropPointGroup::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    const auto savedPos = sio.getPosition();
    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(savedPos);
        return Persistent::Error::ReadError;
    }
    bool r = false;
    if (!sio.read(r)) {
        sio.setPosition(savedPos);
        return Persistent::Error::ReadError;
    }
    random_ = r;
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

Persistent::Error DropPointGroup::write(StreamIO& sio,
                                        Int32 version,
                                        Int32 user,
                                        Persistent::StoreContext& ctx) const {
    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::WriteError;
    }
    sio.write(random_);
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

} // namespace opent1::game
