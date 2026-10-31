#pragma once
#include <opent1/game/data_block_manager.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace opent1::game {

class FearDynamicDataPlugin final : public sim::SimConsolePlugin, public sim::SimDataBlockManager {
public:
    enum FieldTypeId : Int32 {
        TypeInt = 0,
        TypeFloat = 1,
        TypeString = 2,
        TypeBool = 3,
        TypePoint3F = 4,
        TypeColorF = 5,
        TypeSoundFlags = 6,
        TypeNormalVector = 7,
        TypeDamageSkinBlock = 8,
        TypeSoundProfileBlock = 9,
        TypeSoundBlock = 10,
        TypeExplosionBlock = 11,
        TypeDebrisBlock = 12,
        TypeItemImageBlock = 13,
        TypeAnimData = 14,
        TypeSequenceSound = 15,
        TypeProjectileBlock = 16,
        TypeGrenadeDist = 17,
        TypeStaticDamage = 18,
        TypeLightAnimParam = 19,
        TypeWheel = 20,
        TypeSensorBlock = 21,
        TypeTurretBlock = 22,
        TypeMarkerBlock = 23,
        TypeItemBlock = 24
    };

    struct FieldEntry {
        std::string name;
        FieldTypeId type{TypeString};
        std::size_t arraySize{1};
        std::function<std::string(const GameBaseData&, std::size_t)> getter;
        std::function<bool(
            GameBaseData&, std::size_t, std::span<const std::string>, FearDynamicDataPlugin&)>
            setter;
    };

    struct ClassSchema {
        std::string name;
        std::string superClassName;
        Int32 groupId{-1};
        std::size_t hashTableSize{};
        std::vector<FieldEntry> fields;
        std::vector<std::vector<std::size_t>> hashBuckets;
    };

    explicit FearDynamicDataPlugin(std::shared_ptr<ResourceManager> resources = {},
                                   std::filesystem::path writeRoot = {});
    ~FearDynamicDataPlugin() override = default;

    // SimConsolePlugin interface
    void init(Console& console, sim::SimManager* manager) override;
    void shutdown() noexcept override;
    void setManager(sim::SimManager* manager) override;

    // SimDataBlockManager / ConsoleDataManager interface
    [[nodiscard]] bool contains(std::string_view name) const override;
    void declareDataBlock(std::string_view className, std::string_view name) override;
    void endDataBlock() override;
    void abortDataBlock() override;
    std::string getDataField(std::string_view objectName,
                             std::string_view fieldName,
                             std::optional<std::string_view> arrayIndex) override;
    void setDataField(std::optional<std::string_view> objectName,
                      std::string_view fieldName,
                      std::optional<std::string_view> arrayIndex,
                      std::span<const std::string> values) override;

    // Accessors and helpers
    [[nodiscard]] DataBlockManager* activeDataBlockManager() const noexcept;

    void setFallbackDataBlockManager(std::shared_ptr<DataBlockManager> dbm) {
        fallbackDbm_ = std::move(dbm);
    }

    [[nodiscard]] DataBlockManager& ensureDataBlockManager();

    [[nodiscard]] const ClassSchema* findClassSchema(std::string_view className) const noexcept;
    [[nodiscard]] const FieldEntry* findClassField(std::string_view className,
                                                   std::string_view fieldName) const noexcept;

    [[nodiscard]] std::span<const ClassSchema> schemas() const noexcept {
        return schemas_;
    }

    [[nodiscard]] Int32 instantGroupId() const noexcept {
        return instantGroupId_;
    }

    [[nodiscard]] const std::string& instantGroupString() const noexcept {
        return instantGroupStr_;
    }

    [[nodiscard]] std::span<const std::string> exportTextLines() const noexcept {
        return exportLines_;
    }

private:
    struct PendingTransaction {
        DataBlockManager* dbm{};
        DataBlockManager::Snapshot snapshot;
        std::string className;
        std::string blockName;
        Int32 groupId{-1};
        Int32 blockId{-1};
        std::unique_ptr<GameBaseData> previousBlockState;
        bool wasExisting{};
    };

    void registerClasses();
    ClassSchema& registerClass(std::string_view className,
                               std::string_view superClassName,
                               Int32 groupId,
                               const std::function<void(std::vector<FieldEntry>&)>& buildFields);
    [[nodiscard]] GameBaseData*
    resolveTargetBlock(std::optional<std::string_view> objectName) const noexcept;

    std::shared_ptr<ResourceManager> resources_;
    std::filesystem::path writeRoot_;
    Console* console_{};
    sim::SimManager* manager_{};
    std::shared_ptr<DataBlockManager> fallbackDbm_;
    std::vector<ClassSchema> schemas_;
    std::vector<PendingTransaction> transactionStack_;
    std::string instantGroupStr_{"0"};
    Int32 instantGroupId_{};
    std::vector<std::string> exportLines_;

    struct MaterialPropertyOverride {
        Int32 surfaceType{};
        Flt32 friction{1.0f};
        Flt32 elasticity{1.0f};
    };

    std::unordered_map<Int32, MaterialPropertyOverride> materialProps_;
};

} // namespace opent1::game
