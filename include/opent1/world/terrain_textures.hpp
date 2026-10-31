#pragma once
#include <opent1/world/terrain_grid.hpp>
#include <opent1/gfx/texture_cache.hpp>
#include <list>
#include <unordered_map>

namespace opent1::world {
enum class TerrainLightMapDensity { Expanded, Native, NativeOpenGL };
enum class TerrainTextureTiming { RetainedLru, NativeSoftware, NativeOpenGL };

struct TerrainTextureTile {
    Resource<gfx::Bitmap> texture;
    std::shared_ptr<const gfx::LightMap> lightMap;
    Point2F lightMapOrigin{}, lightMapScale{1, 1}; // canonical tile UV -> owned map UV
};

// Bind to immutable resources. Entries own their texture/map; eviction and cache
// destruction never invalidate a previously published tile. External locking is
// required, as for ResourceManager. Byte limits count each cached map once,
// independently of snapshots still retained by callers. Decoded hires grids
// are shared by resident tiles and count against the same byte limit; base
// grids are borrowed from the bound immutable blocks.
class TerrainTextureCache {
public:
    explicit TerrainTextureCache(TerrainGridResources,
                                 std::size_t maximumBytes = 16U * 1024U * 1024U,
                                 UInt32 maximumEntries = 1024);
    TerrainTextureCache(const TerrainTextureCache&);
    TerrainTextureCache& operator=(const TerrainTextureCache&);
    TerrainTextureCache(TerrainTextureCache&&) noexcept = default;
    TerrainTextureCache& operator=(TerrainTextureCache&&) noexcept = default;
    [[nodiscard]] bool matches(const TerrainGridResources&) const;
    [[nodiscard]] bool compatible(UInt32 material, UInt32 other) const;
    // Aligned power-of-two cells and pixels, 1..128 and 1..256 respectively;
    // pixels >= cells; only one-cell direct textures allow one pixel. Textures
    // select authored mip levels (no generated mips).
    // Native uses the original renderer's HRLM falloff/texture cap and
    // requires lightScale == 0 when a baked map is present. Expanded retains
    // the existing up-to-128-interval grid, independently of texture density.
    // dynamicLighting requests the native lit density, retaining an unmerged
    // static base. It requires baked Native or NativeOpenGL lighting; scene
    // merges own copies.
    // NativeOpenGL shares padded block submaps for static tiles, ignores HRLM
    // cells and uses the original OpenGL falloff for dynamic tiles. Native
    // policies require lightScale == 0; OpenGL additionally limits width to 256.
    // Timing is independent of lighting density: native software resizes one
    // logical callback handle and models its pixel ring; native OpenGL retains
    // a current download until eviction, then builds the latest requested size.
    // All modes also obey the owned texture/map byte and entry limits.
    [[nodiscard]] std::optional<TerrainTextureTile>
    tile(UInt32 block,
         UInt32 x,
         UInt32 y,
         UInt32 cells,
         UInt32 pixels = 128,
         bool baked = true,
         bool hires = true,
         TerrainLightMapDensity density = TerrainLightMapDensity::Expanded,
         bool dynamicLighting = false,
         bool nativeEmptyLevels = false,
         TerrainTextureTiming timing = TerrainTextureTiming::RetainedLru);
    void clear();

    [[nodiscard]] std::size_t residentBytes() const {
        return bytes_;
    }

    [[nodiscard]] std::size_t size() const {
        return entries_.size();
    }

    [[nodiscard]] UInt64 builds() const {
        return builds_;
    }

    [[nodiscard]] UInt64 hits() const {
        return hits_;
    }

    [[nodiscard]] UInt64 evictions() const {
        return evictions_;
    }

private:
    friend class TerrainResourceStream;

    struct Key {
        UInt32 block, x, y, cells, pixels;
        bool baked, hires;
        TerrainLightMapDensity density;
        bool dynamicLighting, nativeEmptyLevels;
        TerrainTextureTiming timing;
        bool operator==(const Key&) const = default;
    };

    struct KeyHash {
        std::size_t operator()(const Key&) const noexcept;
    };

    using Samples = std::shared_ptr<const std::vector<UInt16>>;

    struct BuiltTile {
        TerrainTextureTile tile;
        Samples samples;
    };

    struct Entry {
        Key key;
        TerrainTextureTile tile;
        Samples samples;
        std::size_t offset{};
        UInt64 wrap{};
    };

    struct NativeHandle {
        Key requested;
    };

    struct DecodedSamples {
        Samples samples;
        UInt32 references{};
    };

    static Key lookupKey(Key);
    [[nodiscard]] UInt64 sharedMapKey(const Key&) const;
    [[nodiscard]] static bool sharedMap(const Key&);
    [[nodiscard]] std::optional<BuiltTile> build(const Key&) const;
    [[nodiscard]] std::size_t additionalBytes(const BuiltTile&, UInt32 block) const;
    void retain(const Entry&);
    void rebuildIndexes();
    std::list<Entry>::iterator erase(std::list<Entry>::iterator);
    void rebind(TerrainGridResources); // stream only: same immutable grid/materials
    TerrainGridResources resources_;
    std::vector<bool> supported_;
    std::size_t maximumBytes_, bytes_{};
    UInt32 maximumEntries_;
    UInt64 builds_{}, hits_{}, evictions_{};
    std::list<Entry> entries_;
    std::list<NativeHandle> handles_; // native software ring / OpenGL callback residency
    std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> entryIndex_;
    std::unordered_map<Key, std::list<NativeHandle>::iterator, KeyHash> handleIndex_;
    std::unordered_map<const gfx::LightMap*, UInt32> mapReferences_;
    std::unordered_map<UInt64, std::shared_ptr<const gfx::LightMap>> sharedMaps_;
    std::unordered_map<UInt32, DecodedSamples> decodedSamples_;
    std::size_t cursor_{};
    UInt64 wrap_{};
};
} // namespace opent1::world
