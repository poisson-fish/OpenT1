#include <opent1/core/vol_stream.hpp>
#include <ranges>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <opent1/core/rle_stream.hpp>
#include <opent1/core/lzh_stream.hpp>
#include <cstring>
#include <fstream>
#include <chrono>
#include <atomic>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace opent1 {
namespace {
constexpr UInt32 sizeMask = 0x7FFFFFFFU;

UInt32 word(std::span<const std::byte> bytes, std::size_t at) {
    return std::to_integer<UInt32>(bytes[at]) | (std::to_integer<UInt32>(bytes[at + 1]) << 8) |
           (std::to_integer<UInt32>(bytes[at + 2]) << 16) |
           (std::to_integer<UInt32>(bytes[at + 3]) << 24);
}

struct Block {
    std::span<const std::byte> bytes;
    std::size_t end;
};

std::optional<Block> block(std::span<const std::byte> bytes,
                           std::size_t at,
                           UInt32 tag,
                           bool allowMissingTailPadding = false) {
    if (at < 8 || at > bytes.size() || bytes.size() - at < 8 || word(bytes, at) != tag) {
        return {};
    }
    const auto flags = word(bytes, at + 4);
    const auto size = flags & sizeMask;
    const UInt64 mask = (flags & ~sizeMask) != 0 ? 3U : 1U;
    const UInt64 padded = (UInt64(size) + mask) & ~mask;
    const UInt64 end = UInt64(at) + 8 + padded;
    const UInt64 dataEnd = UInt64(at) + 8 + size;
    if (dataEnd > bytes.size() ||
        (end > bytes.size() && (!allowMissingTailPadding || dataEnd != bytes.size()))) {
        return {};
    }
    return Block{bytes.subspan(at + 8, size),
                 static_cast<std::size_t>(std::min(end, UInt64(bytes.size())))};
}

bool sameName(std::string_view a, std::string_view b) {
    const auto fold = [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    };
    return std::ranges::equal(a, b, {}, fold, fold);
}
}

VolumeRStream::VolumeRStream(VolumeRStream&& other) noexcept : VolumeRStream() {
    *this = std::move(other);
}

VolumeRStream& VolumeRStream::operator=(VolumeRStream&& other) noexcept {
    if (this != &other) {
        archive_ = std::move(other.archive_);
        mapped_ = std::move(other.mapped_);
        decoded_ = std::move(other.decoded_);
        entries_ = std::move(other.entries_);
        error_ = std::move(other.error_);
        current_ = std::exchange(other.current_, {});
        position_ = std::exchange(other.position_, 0);
        entryOpen_ = std::exchange(other.entryOpen_, false);
        volumeOpen_ = std::exchange(other.volumeOpen_, false);
        compressed_ = std::exchange(other.compressed_, false);
        strm_status = std::exchange(other.strm_status, STRM_CLOSED);
        lastBytes = std::exchange(other.lastBytes, 0);
    }
    return *this;
}

Bool VolumeRStream::reject(std::string_view message) {
    error_ = message;
    return fail(STRM_IO_ERROR);
}

Bool VolumeRStream::openVolume(const std::filesystem::path& path) {
    closeVolume();
    auto mapping = std::make_unique<ReadOnlyMappedFile>();
    if (mapping->open(path, std::size_t(std::numeric_limits<Int32>::max()))) {
        mapped_ = std::move(mapping);
        return parse();
    }
    FileRStream file(path);
    if (file() != STRM_OK) {
        return reject("Cannot open volume file");
    }
    archive_.resize(file.getSize());
    if (!file.readBytes(archive_)) {
        archive_.clear();
        return reject("Cannot read volume file");
    }
    return parse();
}

Bool VolumeRStream::openVolume(std::span<const std::byte> bytes) {
    // Copy before invalidating possible views into this archive.
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<Int32>::max())) {
        closeVolume();
        return reject("Volume exceeds supported size");
    }
    std::vector<std::byte> temporary(bytes.begin(), bytes.end());
    closeVolume();
    archive_ = std::move(temporary);
    return parse();
}

Bool VolumeRStream::parse() {
    const auto bytes = archiveBytes();
    const auto invalid = [&](std::string_view message) {
        closeVolume();
        return reject(message);
    };
    if (bytes.size() < 8 || word(bytes, 0) != kVolFileId) {
        return invalid("Invalid or obsolete volume signature");
    }
    const auto stringsAt = word(bytes, 4);
    const auto strings = block(bytes, stringsAt, kVolStringTag);
    if (!strings) {
        return invalid("Invalid volume string block");
    }
    const auto dictionary = block(bytes, strings->end, kVolIdsTag, true);
    if (!dictionary || dictionary->bytes.size() % 17 != 0) {
        return invalid("Invalid volume dictionary");
    }
    entries_.reserve(dictionary->bytes.size() / 17);
    bool valid = true;
    std::ranges::for_each(
        std::views::iota(std::size_t{0}, dictionary->bytes.size() / 17), [&](std::size_t i) {
            if (!valid) {
                return;
            }
            const auto record = dictionary->bytes.subspan(i * 17, 17);
            VolumeItem entry;
            entry.id = word(record, 0);
            const auto nameAt = std::bit_cast<Int32>(word(record, 4));
            entry.blockOffset = word(record, 8);
            entry.size = word(record, 12);
            const auto compression = std::to_integer<UInt8>(record[16]);
            if (entry.size > static_cast<UInt32>(std::numeric_limits<Int32>::max()) ||
                (compression != STRM_COMPRESS_NONE && compression != STRM_COMPRESS_RLE &&
                 compression != STRM_COMPRESS_LZH)) {
                valid = false;
                return;
            }
            entry.compression = static_cast<StreamCompressType>(compression);
            const auto payload = block(bytes, entry.blockOffset, kVolFileBlockTag);
            // Dictionary-addressed entries use the actual payload extent. Native
            // archives may omit alignment padding before the directory.
            if (!payload || UInt64(entry.blockOffset) + 8 + payload->bytes.size() > stringsAt ||
                (compression == STRM_COMPRESS_NONE && payload->bytes.size() != entry.size)) {
                valid = false;
                return;
            }
            if (nameAt != -1) {
                if (nameAt < 0 || static_cast<std::size_t>(nameAt) >= strings->bytes.size()) {
                    valid = false;
                    return;
                }
                const auto nameBytes = strings->bytes.subspan(static_cast<std::size_t>(nameAt));
                const auto nul = std::ranges::find(nameBytes, std::byte{0});
                if (nul == nameBytes.end()) {
                    valid = false;
                    return;
                }
                entry.name = std::string(reinterpret_cast<const char*>(nameBytes.data()),
                                         static_cast<std::size_t>(nul - nameBytes.begin()));
            }
            entries_.push_back(std::move(entry));
        });
    if (!valid) {
        return invalid("Invalid volume entry bounds, name, or compression type");
    }
    volumeOpen_ = true;
    clrStatus();
    return true;
}

void VolumeRStream::close() {
    current_ = {};
    decoded_.clear();
    position_ = 0;
    lastBytes = 0;
    entryOpen_ = compressed_ = false;
    strm_status = STRM_CLOSED;
}

void VolumeRStream::closeVolume() {
    close();
    archive_.clear();
    std::vector<std::byte>{}.swap(archive_);
    std::vector<std::byte>{}.swap(decoded_);
    mapped_.reset();
    entries_.clear();
    error_.clear();
    volumeOpen_ = false;
}

Bool VolumeRStream::open(std::string_view name, UInt32 maxDecodedBytes) {
    const auto it = std::ranges::find_if(entries_, [&](const auto& entry) {
        return entry.name && sameName(*entry.name, name);
    });
    return openEntry(static_cast<std::size_t>(it - entries_.begin()), maxDecodedBytes);
}

Bool VolumeRStream::open(UInt32 id, UInt32 maxDecodedBytes) {
    const auto it = std::ranges::find_if(entries_, [&](const auto& entry) {
        return id != 0 && entry.id == id;
    });
    return openEntry(static_cast<std::size_t>(it - entries_.begin()), maxDecodedBytes);
}

Bool VolumeRStream::openEntry(std::size_t index, UInt32 maxDecodedBytes) {
    close();
    error_.clear();
    if (!volumeOpen_ || index >= entries_.size()) {
        return reject("Volume entry not found");
    }
    const auto& entry = entries_[index];
    if (entry.size > maxDecodedBytes) {
        return reject("Volume entry exceeds decoded size limit");
    }
    if (mapped_ && !mapped_->unchanged())
        return reject("Mounted volume changed in place");
    // parse() validated every block; backing remains immutable while mounted.
    const auto payload = block(archiveBytes(), entry.blockOffset, kVolFileBlockTag)->bytes;
    if (entry.compression == STRM_COMPRESS_NONE) {
        current_ = payload;
    } else {
        MemRWStream parent(payload);
        RLERStream rle;
        LZHRStream lzh;
        StreamIO* reader;
        if (entry.compression == STRM_COMPRESS_RLE) {
            rle.attach(parent);
            reader = &rle;
        } else {
            lzh.attach(parent);
            reader = &lzh;
        }
        decoded_.resize(entry.size);
        if (!reader->readBytes(decoded_)) {
            decoded_.clear();
            return reject("Truncated compressed volume entry");
        }
        current_ = decoded_;
    }
    entryOpen_ = true;
    compressed_ = entry.compression != STRM_COMPRESS_NONE;
    clrStatus();
    return true;
}

Bool VolumeRStream::setPosition(Int32 position) {
    if (!entryOpen_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status == STRM_EOS) {
        clrStatus();
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    if (position < 0 || static_cast<std::size_t>(position) > current_.size()) {
        return fail(STRM_ILLEGAL_CALL);
    }
    position_ = position;
    return true;
}

Int32 VolumeRStream::getCapabilities(StreamCap cap) const {
    switch (cap) {
        case STRM_CAP_TYPE:
            return STRM_MEM_R;
        case STRM_CAP_READ:
        case STRM_CAP_REPOSITION:
            return entryOpen_;
        case STRM_CAP_COMPRESS:
            return entryOpen_ && compressed_;
        default:
            return 0;
    }
}

Bool VolumeRStream::readBytes(std::span<std::byte> bytes) {
    lastBytes = 0;
    if (!entryOpen_) {
        return fail(STRM_CLOSED);
    }
    if (strm_status != STRM_OK) {
        return false;
    }
    const auto remaining = current_.subspan(static_cast<std::size_t>(position_));
    const auto count = std::min(bytes.size(), remaining.size());
    if (count != 0) {
        std::memmove(bytes.data(), remaining.data(), count);
    }
    lastBytes = static_cast<UInt32>(count);
    position_ += static_cast<Int32>(count);
    return count == bytes.size() || fail(STRM_EOS);
}

bool VolumeRWStream::openVolume(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > MaxArchiveBytes) {
        return false;
    }
    FileRStream input(path);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    return input.readBytes(bytes) && openVolume(bytes);
}

bool VolumeRWStream::openVolume(std::span<const std::byte> bytes) {
    if (bytes.size() > MaxArchiveBytes) {
        return false;
    }
    VolumeRStream input;
    if (!input.openVolume(bytes)) {
        return false;
    }
    std::vector<Entry> next;
    std::size_t total = 0;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < input.getVolumeItems().size();
                  })) {
        const auto& item = input.getVolumeItems()[i];
        if (item.size > MaxArchiveBytes - total ||
            !input.openEntry(i, static_cast<UInt32>(MaxArchiveBytes - total))) {
            return false;
        }
        if (std::ranges::any_of(next, [&](const auto& old) {
                return (item.id && old.id == item.id) ||
                       (item.name && old.name && sameName(*item.name, *old.name));
            })) {
            return false;
        }
        next.push_back(
            {item.id, item.name, item.compression, {input.data().begin(), input.data().end()}});
        total += item.size;
    }
    entries_ = std::move(next);
    return true;
}

bool VolumeRWStream::replace(Entry entry) {
    if ((!entry.name && !entry.id) || entry.bytes.size() > MaxArchiveBytes ||
        (entry.compression != STRM_COMPRESS_NONE && entry.compression != STRM_COMPRESS_RLE &&
         entry.compression != STRM_COMPRESS_LZH)) {
        return false;
    }
    auto found = std::ranges::find_if(entries_, [&](const auto& old) {
        return entry.name ? old.name && sameName(*entry.name, *old.name) : entry.id == old.id;
    });
    std::size_t total = entry.bytes.size();
    for (const auto& old : entries_) {
        if (found != entries_.end() && &old == &*found) {
            continue;
        }
        if (entry.id && old.id == entry.id) {
            return false;
        }
        if (old.bytes.size() > MaxArchiveBytes - total) {
            return false;
        }
        total += old.bytes.size();
    }
    if (found == entries_.end()) {
        entries_.push_back(std::move(entry));
    } else {
        *found = std::move(entry);
    }
    return true;
}

bool VolumeRWStream::put(std::string_view name,
                         std::span<const std::byte> bytes,
                         StreamCompressType compression,
                         UInt32 id) {
    if (name.empty() || name.size() > 255 || name.find('\0') != name.npos ||
        bytes.size() > MaxArchiveBytes) {
        return false;
    }
    if (!id) {
        const auto old = std::ranges::find_if(entries_, [&](const auto& entry) {
            return entry.name && sameName(*entry.name, name);
        });
        if (old != entries_.end()) {
            id = old->id;
        }
    }
    return replace({id, std::string(name), compression, {bytes.begin(), bytes.end()}});
}

bool VolumeRWStream::put(UInt32 id,
                         std::span<const std::byte> bytes,
                         StreamCompressType compression) {
    if (!id || bytes.size() > MaxArchiveBytes) {
        return false;
    }
    const auto old = std::ranges::find(entries_, id, &Entry::id);
    return replace({id,
                    old == entries_.end() ? std::nullopt : old->name,
                    compression,
                    {bytes.begin(), bytes.end()}});
}

bool VolumeRWStream::remove(std::string_view name) {
    return std::erase_if(entries_, [&](const auto& entry) {
               return entry.name && sameName(*entry.name, name);
           }) != 0;
}

bool VolumeRWStream::remove(UInt32 id) {
    return id && std::erase_if(entries_, [&](const auto& entry) {
                     return entry.id == id;
                 }) != 0;
}

std::optional<std::vector<std::byte>> VolumeRWStream::encode() const {
    std::vector<std::byte> output, names, dictionary;
    const auto putWord = [](std::vector<std::byte>& target, UInt32 value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            target.push_back(static_cast<std::byte>((value >> shift) & 255));
        }
    };
    const auto putBlock = [&](UInt32 tag, std::span<const std::byte> bytes) {
        putWord(output, tag);
        putWord(output, static_cast<UInt32>(bytes.size()));
        output.insert(output.end(), bytes.begin(), bytes.end());
        if (bytes.size() & 1) {
            output.push_back(std::byte{0});
        }
    };
    putWord(output, kVolFileId);
    putWord(output, 0);
    for (const auto& entry : entries_) {
        std::vector<std::byte> payload;
        if (entry.compression == STRM_COMPRESS_NONE) {
            payload = entry.bytes;
        } else {
            payload.resize(entry.bytes.size() * 2 + 1024);
            MemRWStream stream{std::span(payload)};
            bool ok = false;
            if (entry.compression == STRM_COMPRESS_RLE) {
                RLEWStream encoder(stream);
                ok = encoder.writeBytes(entry.bytes) && encoder.flush();
                encoder.detach();
            } else {
                LZHWStream encoder(stream);
                ok = encoder.writeBytes(entry.bytes) && encoder.flush();
            }
            if (!ok) {
                return {};
            }
            payload.resize(static_cast<std::size_t>(stream.getPosition()));
        }
        if (payload.size() + output.size() + names.size() + dictionary.size() + 300 >
            MaxArchiveBytes) {
            return {};
        }
        putWord(dictionary, entry.id);
        putWord(dictionary, entry.name ? static_cast<UInt32>(names.size()) : ~UInt32{0});
        putWord(dictionary, static_cast<UInt32>(output.size()));
        putWord(dictionary, static_cast<UInt32>(entry.bytes.size()));
        dictionary.push_back(static_cast<std::byte>(entry.compression));
        if (entry.name) {
            const auto data = std::as_bytes(std::span(*entry.name));
            names.insert(names.end(), data.begin(), data.end());
            names.push_back(std::byte{0});
        }
        putBlock(kVolFileBlockTag, payload);
    }
    const auto stringsOffset = static_cast<UInt32>(output.size());
    for (auto byte : std::views::iota(unsigned{0}, unsigned{4})) {
        output[4 + byte] = static_cast<std::byte>((stringsOffset >> (byte * 8)) & 255);
    }
    putBlock(kVolStringTag, names);
    putBlock(kVolIdsTag, dictionary);
    return output;
}

bool VolumeRWStream::save(const std::filesystem::path& destination) const {
    const auto bytes = encode();
    if (!bytes) {
        return false;
    }
    static std::atomic<UInt64> serial{};
    auto temporary = destination;
    temporary += ".saving-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(serial.fetch_add(1));
    {
        FileWStream output(temporary);
        if (!output.writeBytes(*bytes) || !output.flush()) {
            output.close();
            std::error_code cleanup;
            std::filesystem::remove(temporary, cleanup);
            return false;
        }
    }
#if defined(_WIN32)
    bool success = MoveFileExW(temporary.c_str(),
                               destination.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!success) {
        // Mapped readers keep the old file object. POSIX replacement updates the
        // name while preserving those readers; unsupported filesystems use buffered I/O.
        // https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/4217551b-d2c0-42cb-9dc1-69a716cf6d0c
        const auto file = CreateFileW(temporary.c_str(),
                                      GENERIC_WRITE | DELETE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            std::error_code ec;
            const auto target = std::filesystem::absolute(destination, ec).native();
            const auto nameBytes = target.size() * sizeof(wchar_t);
            // The Win32 rename path can inspect a trailing NUL even with an
            // explicit FileNameLength. Include it rather than relying on the
            // alignment padding of the allocation to terminate the filename.
            const auto bytesNeeded =
                offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);
            if (!ec && bytesNeeded <= std::numeric_limits<DWORD>::max() && FlushFileBuffers(file)) {
                std::vector<UInt64> storage((bytesNeeded + sizeof(UInt64) - 1) / sizeof(UInt64));
                auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
                rename->Flags =
                    FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
                rename->FileNameLength = DWORD(nameBytes);
                std::memcpy(rename->FileName, target.data(), nameBytes);
                success = SetFileInformationByHandle(
                              file, FileRenameInfoEx, rename, DWORD(bytesNeeded)) != 0;
            }
            CloseHandle(file);
        }
    }
#else
    std::error_code renameError;
    std::filesystem::rename(temporary, destination, renameError);
    const bool success = !renameError;
#endif
    if (!success) {
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
    }
    return success;
}
} // namespace opent1
