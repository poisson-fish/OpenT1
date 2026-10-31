#pragma once
#include <opent1/core/stream_io.hpp>
#include <filesystem>
#include <optional>
#include <vector>

namespace opent1 {
inline constexpr UInt32 kVolFileId = makeFourCC('P', 'V', 'O', 'L');
inline constexpr UInt32 kVolStringTag = makeFourCC('v', 'o', 'l', 's');
inline constexpr UInt32 kVolIdsTag = makeFourCC('v', 'o', 'l', 'i');
inline constexpr UInt32 kVolFileBlockTag = makeFourCC('V', 'B', 'L', 'K');

// Host representation only. The on-disk dictionary is exactly 17 bytes/entry;
// no native structure or pointer is ever serialized/deserialized.
struct VolumeItem {
    UInt32 id{};
    std::optional<std::string> name;
    UInt32 blockOffset{}, size{};
    StreamCompressType compression{STRM_COMPRESS_NONE};
};

// Owns archive bytes and decoded entries. Names compare with ASCII case folding.
// Entry views expire on close/open; metadata views expire on close/openVolume.
// Disk search paths belong in higher-level APIs.
class VolumeRStream : public StreamIO {
public:
    VolumeRStream() {
        strm_status = STRM_CLOSED;
    }

    VolumeRStream(const VolumeRStream&) = delete;
    VolumeRStream& operator=(const VolumeRStream&) = delete;
    VolumeRStream(VolumeRStream&& other) noexcept;
    VolumeRStream& operator=(VolumeRStream&& other) noexcept;
    Bool openVolume(const std::filesystem::path& path);
    Bool openVolume(std::span<const std::byte> bytes);
    void closeVolume();

    [[nodiscard]] std::span<const VolumeItem> getVolumeItems() const noexcept {
        return entries_;
    }

    [[nodiscard]] std::string_view error() const noexcept {
        return error_;
    }

    Bool open(std::string_view name, UInt32 maxDecodedBytes = 64U * 1024U * 1024U);
    Bool open(UInt32 id, UInt32 maxDecodedBytes = 64U * 1024U * 1024U);
    Bool openEntry(std::size_t index, UInt32 maxDecodedBytes = 64U * 1024U * 1024U);
    void close() override;

    [[nodiscard]] std::span<const std::byte> data() const noexcept {
        return current_;
    }

    [[nodiscard]] UInt32 getSize() const noexcept {
        return static_cast<UInt32>(current_.size());
    }

    [[nodiscard]] Int32 getPosition() const override {
        return position_;
    }

    Bool setPosition(Int32 position) override;
    [[nodiscard]] Int32 getCapabilities(StreamCap cap) const override;
    Bool readBytes(std::span<std::byte> bytes) override;

private:
    Bool parse();
    Bool reject(std::string_view message);
    std::vector<std::byte> archive_, decoded_;
    std::vector<VolumeItem> entries_;
    std::span<const std::byte> current_;
    std::string error_;
    Int32 position_{0};
    bool entryOpen_{false}, volumeOpen_{false}, compressed_{false};
};

// Transactional PVOL authoring. Edits stay in memory until save(); encode()
// compacts the archive, preserving numeric IDs, names and compression modes.
// A failed open or edit leaves the previous archive intact.
class VolumeRWStream {
public:
    struct Entry {
        UInt32 id{};
        std::optional<std::string> name;
        StreamCompressType compression{STRM_COMPRESS_NONE};
        std::vector<std::byte> bytes;
    };

    static constexpr std::size_t MaxArchiveBytes = 256U * 1024U * 1024U;
    bool openVolume(const std::filesystem::path&);
    bool openVolume(std::span<const std::byte>);

    void createVolume() {
        entries_.clear();
    }

    bool put(std::string_view name,
             std::span<const std::byte>,
             StreamCompressType = STRM_COMPRESS_NONE,
             UInt32 id = 0);
    bool put(UInt32 id, std::span<const std::byte>, StreamCompressType = STRM_COMPRESS_NONE);
    bool remove(std::string_view name);
    bool remove(UInt32 id);

    [[nodiscard]] std::span<const Entry> entries() const {
        return entries_;
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> encode() const;
    bool save(const std::filesystem::path&) const;

private:
    bool replace(Entry);
    std::vector<Entry> entries_;
};
} // namespace opent1
