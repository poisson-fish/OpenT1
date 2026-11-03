// Read-only asset/mission measurements. Replacement new/delete are linked only
// into this optional tool. Counts requested C++ allocation bytes, not allocator
// metadata, malloc in shared libraries, process RSS, or GPU allocations.
#include <opent1/core/file_stream.hpp>
#include <iterator>
#include <span>
#include <ranges>
#include <opent1/core/vol_stream.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/observer_camera.hpp>
#include <opent1/world/world_mission.hpp>
#include <opent1/audio/audio.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <set>
#include <type_traits>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#endif
#if defined(OPENT1_MEMORY_GPU)
#include "sdl_gpu_material.hpp"
#include <SDL3/SDL.h>
#endif

namespace meter {
enum Tag : unsigned {
    Other,
    Shape,
    Material,
    Interior,
    Geometry,
    Lighting,
    Grid,
    Block,
    Bitmap,
    Palette,
    Font,
    BitmapArray,
    Frame,
    Wave,
    Count
};

constexpr const char* names[]{"other",
                              "shape",
                              "material",
                              "interior",
                              "geometry",
                              "lighting",
                              "grid",
                              "block",
                              "bitmap",
                              "palette",
                              "font",
                              "bitmap_array",
                              "frame",
                              "wave"};

struct Counter {
    std::atomic<std::size_t> live{}, peak{}, allocations{}, blocks{};
};

constinit Counter counters[Count], total;
thread_local Tag tag = Other;

struct Scope {
    Tag previous{tag};

    explicit Scope(Tag next) {
        tag = next;
    }

    ~Scope() {
        tag = previous;
    }
};

struct Metrics {
    std::uint64_t vertices{}, triangles{}, surfaces{}, heightSamples{}, terrainCells{},
        blockPlacements{}, bitmapPixels{}, rgbaBytes{}, lightRgbaBytes{}, pcmBytes{};

    Metrics& operator+=(const Metrics& b) {
        vertices += b.vertices;
        triangles += b.triangles;
        surfaces += b.surfaces;
        heightSamples += b.heightSamples;
        terrainCells += b.terrainCells;
        blockPlacements += b.blockPlacements;
        bitmapPixels += b.bitmapPixels;
        rgbaBytes += b.rgbaBytes;
        lightRgbaBytes += b.lightRgbaBytes;
        pcmBytes += b.pcmBytes;
        return *this;
    }
};

struct Record {
    Record* next;
    std::atomic<bool> live{true};
    Tag category;
    Metrics metrics;
};

constinit std::atomic<Record*> records{};

struct Header {
    void* allocation;
    std::size_t bytes;
    Tag category;
    Record* record;
};

void add(Counter& c, std::size_t bytes) {
    const auto now = c.live.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    auto peak = c.peak.load(std::memory_order_relaxed);
    while (peak < now && !c.peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
    }
    c.allocations.fetch_add(1, std::memory_order_relaxed);
    c.blocks.fetch_add(1, std::memory_order_relaxed);
}

void* allocate(std::size_t bytes, std::size_t alignment) {
    alignment = std::max(alignment, alignof(Header));
    if (bytes > SIZE_MAX - sizeof(Header) - alignment)
        throw std::bad_alloc();
    void* raw{};
    while (!(raw = std::malloc(std::max<std::size_t>(1, bytes) + sizeof(Header) + alignment))) {
        if (const auto handler = std::get_new_handler())
            handler();
        else
            throw std::bad_alloc();
    }
    const auto address = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(Header) + alignment - 1) &
                         ~(std::uintptr_t(alignment) - 1);
    auto* h = reinterpret_cast<Header*>(address) - 1;
    *h = {raw, bytes, tag, nullptr};
    add(counters[tag], bytes);
    add(total, bytes);
    return reinterpret_cast<void*>(address);
}

void release(void* pointer) noexcept {
    if (!pointer)
        return;
    auto* h = static_cast<Header*>(pointer) - 1;
    if (h->record)
        h->record->live.store(false, std::memory_order_relaxed);
    counters[h->category].live.fetch_sub(h->bytes, std::memory_order_relaxed);
    counters[h->category].blocks.fetch_sub(1, std::memory_order_relaxed);
    total.live.fetch_sub(h->bytes, std::memory_order_relaxed);
    total.blocks.fetch_sub(1, std::memory_order_relaxed);
    std::free(h->allocation);
}

void attach(void* pointer, Tag category, Metrics metrics) {
    auto* raw = std::malloc(sizeof(Record));
    if (!raw)
        throw std::bad_alloc();
    auto* record = ::new (raw) Record{};
    record->category = category;
    record->metrics = metrics;
    record->next = records.load();
    while (!records.compare_exchange_weak(record->next, record)) {
    }
    (static_cast<Header*>(pointer) - 1)->record = record;
}
} // namespace meter

void* operator new(std::size_t n) {
    return meter::allocate(n, alignof(std::max_align_t));
}

void* operator new[](std::size_t n) {
    return ::operator new(n);
}

void operator delete(void* p) noexcept {
    meter::release(p);
}

void operator delete[](void* p) noexcept {
    meter::release(p);
}

void operator delete(void* p, std::size_t) noexcept {
    meter::release(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    meter::release(p);
}

void* operator new(std::size_t n, std::align_val_t a) {
    return meter::allocate(n, std::size_t(a));
}

void* operator new[](std::size_t n, std::align_val_t a) {
    return ::operator new(n, a);
}

void operator delete(void* p, std::align_val_t) noexcept {
    meter::release(p);
}

void operator delete[](void* p, std::align_val_t) noexcept {
    meter::release(p);
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    meter::release(p);
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    meter::release(p);
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(n);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    meter::release(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    meter::release(p);
}

void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(n, a);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t& t) noexcept {
    return ::operator new(n, a, t);
}

void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    meter::release(p);
}

void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    meter::release(p);
}

using namespace opent1;

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void quote(std::ostream& out, std::string_view value) {
    out << '"';
    constexpr char hex[]{"0123456789abcdef"};
    for (unsigned char c : value) {
        if (c == '"' || c == '\\')
            out << '\\' << char(c);
        else if (c < 32)
            out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else
            out << char(c);
    }
    out << '"';
}

void metrics(std::ostream& out, const meter::Metrics& m) {
    out << ",\"vertices\":" << m.vertices << ",\"triangles\":" << m.triangles
        << ",\"surfaces\":" << m.surfaces << ",\"height_samples\":" << m.heightSamples
        << ",\"terrain_cells\":" << m.terrainCells << ",\"block_placements\":" << m.blockPlacements
        << ",\"bitmap_pixels_bytes\":" << m.bitmapPixels << ",\"rgba_bytes\":" << m.rgbaBytes
        << ",\"light_rgba_bytes\":" << m.lightRgbaBytes << ",\"pcm_bytes\":" << m.pcmBytes;
}

[[maybe_unused]] void videoMemory(std::ostream& out) {
#if defined(_WIN32)
    IDXGIFactory1* factory{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return;
    UInt64 local{}, shared{};
    for (auto index : std::views::iota(UINT{0})) {
        IDXGIAdapter1* adapter{};
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        if (!adapter)
            break;
        IDXGIAdapter3* memory{};
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&memory)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(memory->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
                local += info.CurrentUsage;
            if (SUCCEEDED(
                    memory->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info)))
                shared += info.CurrentUsage;
            memory->Release();
        }
        adapter->Release();
    }
    factory->Release();
    out << ",\"dxgi_local_bytes\":" << local << ",\"dxgi_nonlocal_bytes\":" << shared;
#else
    (void)out;
#endif
}

meter::Metrics inspect(const gfx::Bitmap& b) {
    meter::Metrics m;
    m.bitmapPixels = b.pixels.size();
    for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                      return i < b.detailLevels;
                  }))
        m.rgbaBytes += UInt64(std::max(1, b.width >> i)) * UInt32(std::max(1, b.height >> i)) * 4;
    return m;
}

template <class T> meter::Metrics inspect(const T& t) {
    meter::Metrics m;
    if constexpr (std::is_same_v<T, audio::Wave>) {
        m.pcmBytes = t.samples.size();
    } else if constexpr (std::is_same_v<T, world::TsShape>) {
        for (const auto& mesh : t.meshes) {
            m.vertices += mesh.vertices.size();
            m.triangles += mesh.faces.size();
        }
    } else if constexpr (std::is_same_v<T, world::InteriorGeometry>) {
        m.vertices = t.points.size();
        m.surfaces = t.surfaces.size();
        for (const auto& s : t.surfaces)
            if (s.renderable() && s.vertexCount >= 3)
                m.triangles += s.vertexCount - 2;
    } else if constexpr (std::is_same_v<T, world::InteriorLighting>) {
        for (const auto& s : t.surfaces)
            m.lightRgbaBytes +=
                UInt64(std::max(2U, unsigned(s.width))) * std::max(2U, unsigned(s.height)) * 4;
    } else if constexpr (std::is_same_v<T, world::TerrainGridBlock>) {
        m.heightSamples = t.heights.size();
        m.terrainCells = t.materials.size();
    } else if constexpr (std::is_same_v<T, world::TerrainGridFile>) {
        m.blockPlacements = std::ranges::count_if(t.blockMap, [](auto id) {
            return id >= 0;
        });
    } else if constexpr (std::is_same_v<T, gfx::BitmapArray>) {
        for (const auto& b : t.bitmaps)
            m += inspect(b);
    } else if constexpr (std::is_same_v<T, gfx::BitmapFont>) {
        m += inspect(t.sheets);
    }
    return m;
}

template <class T> void loader(ResourceManager& r, std::string_view extension, meter::Tag tag) {
    require(r.registerLoader<T>(
                extension,
                [tag](StreamIO& stream, UInt32 size) {
                    meter::Scope scope(tag);
                    auto value = T::load(stream, size);
                    if (value)
                        meter::attach(value.get(), tag, inspect(*value));
                    return value;
                },
                kResDefaultTimeout,
                true),
            "register measured loader");
}

void loaders(ResourceManager& r) {
    loader<world::TsShape>(r, ".dts", meter::Shape);
    loader<world::TsMaterialList>(r, ".dml", meter::Material);
    loader<world::InteriorShape>(r, ".dis", meter::Interior);
    loader<world::InteriorGeometry>(r, ".dig", meter::Geometry);
    loader<world::InteriorLighting>(r, ".dil", meter::Lighting);
    loader<world::TerrainGridFile>(r, ".dtf", meter::Grid);
    loader<world::TerrainGridBlock>(r, ".dtb", meter::Block);
    for (auto e : {".bmp", ".dib", ".pbm", ".png"})
        loader<gfx::Bitmap>(r, e, meter::Bitmap);
    for (auto e : {".pal", ".ppl", ".dpl"})
        loader<gfx::Palette>(r, e, meter::Palette);
    loader<gfx::BitmapArray>(r, ".pba", meter::BitmapArray);
    loader<gfx::BitmapFont>(r, ".pft", meter::Font);
    loader<audio::Wave>(r, ".wav", meter::Wave);
}

void snapshot(std::ostream& out, std::string_view stage) {
    // Capture counters before formatting the record allocates stream buffers.
    const auto live = meter::total.live.load(), peak = meter::total.peak.load();
    std::size_t processPrivate{}, workingSet{}, peakWorkingSet{};
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX p{};
    p.cb = sizeof(p);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p), sizeof(p))) {
        processPrivate = p.PrivateUsage;
        workingSet = p.WorkingSetSize;
        peakWorkingSet = p.PeakWorkingSetSize;
    }
#endif
    out << "{\"kind\":\"snapshot\",\"stage\":";
    quote(out, stage);
    out << ",\"cpp_live_bytes\":" << live << ",\"cpp_peak_bytes\":" << peak
        << ",\"cpp_live_allocations\":" << meter::total.blocks.load()
        << ",\"process_private_bytes\":" << processPrivate
        << ",\"working_set_bytes\":" << workingSet
        << ",\"peak_working_set_bytes\":" << peakWorkingSet << ",\"tags\":{";
    for (auto i : std::views::iota(unsigned{0}) | std::views::take_while([&](unsigned i) {
                      return i < meter::Count;
                  })) {
        if (i)
            out << ',';
        quote(out, meter::names[i]);
        meter::Metrics m;
        std::size_t objects{};
        for (auto* r = meter::records.load(); r; r = r->next) {
            if (r->category == i && r->live.load()) {
                m += r->metrics;
                ++objects;
            }
        }
        out << ":{\"live_bytes\":" << meter::counters[i].live.load()
            << ",\"peak_bytes\":" << meter::counters[i].peak.load()
            << ",\"allocations\":" << meter::counters[i].allocations.load()
            << ",\"objects\":" << objects;
        metrics(out, m);
        out << '}';
    }
    out << "}}\n";
    out.flush();
}

std::string lower(std::string value) {
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return value;
}

template <class T> bool decode(StreamIO& stream, UInt32 size, std::ostream& out) {
    const auto before = meter::counters[meter::Other].live.load();
    const auto peakBefore = meter::total.peak.load();
    auto value = T::load(stream, size);
    const auto after = meter::counters[meter::Other].live.load();
    out << ",\"decoded\":" << (value ? "true" : "false")
        << ",\"decoded_heap_bytes\":" << (after >= before ? after - before : 0);
    if (value)
        metrics(out, inspect(*value));
    value.reset();
    out << ",\"released_heap_bytes\":" << after - meter::counters[meter::Other].live.load()
        << ",\"new_process_highwater_bytes\":" << meter::total.peak.load() - peakBefore;
    return true;
}

void catalogEntry(std::ostream& out,
                  StreamIO& stream,
                  UInt32 size,
                  const std::string& source,
                  std::string_view name) {
    const auto extension = lower(std::filesystem::path(name).extension().string());
    out << "{\"kind\":\"asset\",\"source\":";
    quote(out, source);
    out << ",\"name\":";
    quote(out, name);
    out << ",\"extension\":";
    quote(out, extension);
    out << ",\"source_bytes\":" << size;
    if (extension == ".dts")
        decode<world::TsShape>(stream, size, out);
    else if (extension == ".dml")
        decode<world::TsMaterialList>(stream, size, out);
    else if (extension == ".dis")
        decode<world::InteriorShape>(stream, size, out);
    else if (extension == ".dig")
        decode<world::InteriorGeometry>(stream, size, out);
    else if (extension == ".dil")
        decode<world::InteriorLighting>(stream, size, out);
    else if (extension == ".dtf")
        decode<world::TerrainGridFile>(stream, size, out);
    else if (extension == ".dtb")
        decode<world::TerrainGridBlock>(stream, size, out);
    else if (extension == ".bmp" || extension == ".dib" || extension == ".pbm" ||
             extension == ".png")
        decode<gfx::Bitmap>(stream, size, out);
    else if (extension == ".pal" || extension == ".ppl" || extension == ".dpl")
        decode<gfx::Palette>(stream, size, out);
    else if (extension == ".pft")
        decode<gfx::BitmapFont>(stream, size, out);
    else if (extension == ".pba")
        decode<gfx::BitmapArray>(stream, size, out);
    else if (extension == ".wav")
        decode<audio::Wave>(stream, size, out);
    out << "}\n";
}

void catalog(const std::filesystem::path& root,
             std::ostream& out,
             std::span<const std::string_view> directories) {
    std::vector<std::filesystem::path> files;
    for (const auto directory : directories)
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / directory))
            if (entry.is_regular_file())
                files.push_back(entry.path());
    std::ranges::sort(files);
    for (const auto& file : files) {
        const auto source = std::filesystem::relative(file, root).generic_string();
        const auto extension = lower(file.extension().string());
        if (extension == ".vol" || extension == ".ted") {
            VolumeRStream volume;
            if (!volume.openVolume(file)) {
                out << "{\"kind\":\"error\",\"source\":";
                quote(out, source);
                out << "}\n";
                continue;
            }
            out << "{\"kind\":\"archive\",\"source\":";
            quote(out, source);
            out << ",\"disk_bytes\":" << std::filesystem::file_size(file)
                << ",\"entries\":" << volume.getVolumeItems().size() << "}\n";
            for (auto i : std::views::iota(std::size_t{0}, volume.getVolumeItems().size())) {
                const auto& entry = volume.getVolumeItems()[i];
                if (!entry.name)
                    continue;
                if (volume.openEntry(i))
                    catalogEntry(out, volume, entry.size, source, *entry.name);
                else {
                    out << "{\"kind\":\"error\",\"source\":";
                    quote(out, source);
                    out << ",\"name\":";
                    quote(out, *entry.name);
                    out << "}\n";
                }
            }
        } else {
            FileRStream stream(file);
            if (stream() == STRM_OK)
                catalogEntry(out, stream, stream.getSize(), source, file.filename().string());
        }
    }
}

void mission(const std::filesystem::path& root,
             std::string_view name,
             const std::filesystem::path& output,
             std::ostream& out,
             bool rpg,
             unsigned frames,
             bool gpu) {
    game::FearGameApp::Config cfg;
    cfg.assetRoot = root;
    cfg.writeRoot = std::filesystem::absolute(
        output.string() + "-runtime-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    game::FearGameApp app(cfg);
    std::vector<std::string_view> args;
    if (rpg)
        args = {"-mod", "RPG"};
    loaders(app.resources());
    require(app.init(args), "application init failed");
    app.console().setVariable("Server::Port", "0");
    app.console().setVariable("Server::Heartbeats", "false");
    app.console().setVariable("Server::warmupTime", "0");
    app.console().setVariable("PCFG::Name", "MemoryAudit");
    app.console().setVariable("PCFG::Info", "local-measurement");
    app.console().setVariable("PCFG::Script", "");
    snapshot(out, "startup");
    require(app.loadMission(name), "mission load failed");
    snapshot(out, "mission_loaded");
    out << "{\"kind\":\"mounts\",\"volumes\":[";
    bool comma{};
    UInt64 archiveBytes{};
    for (const auto& volume : app.resources().mountedVolumes()) {
        if (comma)
            out << ',';
        comma = true;
        quote(out, volume);
        if (const auto path = app.resources().getFullPath(volume))
            archiveBytes += std::filesystem::file_size(*path);
    }
    const auto archiveMemory = app.resources().archiveMemoryStats();
    out << "],\"archive_file_bytes\":" << archiveBytes
        << ",\"archive_mapped_bytes\":" << archiveMemory.mappedBytes
        << ",\"archive_owned_buffer_bytes\":" << archiveMemory.ownedBufferBytes
        << ",\"archive_decode_staging_bytes\":" << archiveMemory.decodeStagingBytes << "}\n";
    for (auto i : std::views::iota(unsigned{0}, unsigned{8}))
        require(app.stepFrame((i + 1) * .032, false), "warmup failed");
    snapshot(out, "simulation_warm");
    const auto& client = app.fearPlugin().clientGlobals();
    const auto& server = app.fearPlugin().serverGlobals();
    out << "{\"kind\":\"context\",\"client_control\":"
        << (client.playerPSC && client.playerPSC->getControlObject() ? "true" : "false")
        << ",\"server_control\":"
        << (server.playerPSC && server.playerPSC->getControlObject() ? "true" : "false") << "}\n";
    unsigned worldFrames{};
#if defined(OPENT1_MEMORY_GPU)
    SDL_GPUDevice* device{};
    std::unique_ptr<gfx::detail::GpuMaterialRenderer> renderer;
    if (gpu) {
        require(SDL_Init(SDL_INIT_VIDEO), "SDL init failed");
        device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV |
                                         SDL_GPU_SHADERFORMAT_MSL,
                                     false,
                                     nullptr);
        require(device != nullptr, "GPU device failed");
        renderer = gfx::detail::GpuMaterialRenderer::create(device, 8192);
        require(bool(renderer), "GPU renderer failed");
        out << "{\"kind\":\"device\",\"driver\":";
        quote(out, SDL_GetGPUDeviceDriver(device));
        out << ",\"name\":";
        quote(out,
              SDL_GetStringProperty(
                  SDL_GetGPUDeviceProperties(device), SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown"));
        out << "}\n";
    }
#else
    require(!gpu, "GPU audit requires platform build");
#endif
    if (frames) {
        auto surface = gfx::SoftwareSurface::create(800, 600);
        require(bool(surface), "surface failed");
        auto& cg = app.fearPlugin().clientGlobals();
        auto& sg = app.fearPlugin().serverGlobals();
        auto* observer = cg.playerPSC
                             ? dynamic_cast<game::ObserverCamera*>(cg.playerPSC->getControlObject())
                             : nullptr;
        auto* serverObserver =
            sg.playerPSC ? dynamic_cast<game::ObserverCamera*>(sg.playerPSC->getControlObject())
                         : nullptr;
        const auto pos = observer ? observer->getEyeTransform().p : Point3F{};
        const auto rotation = observer ? observer->rotation() : EulerF{};
        for (auto f : std::views::iota(unsigned{0}) | std::views::take_while([&](unsigned f) {
                          return f < frames;
                      })) {
            require(app.stepFrame((f + 9) * .032, false), "simulation step failed");
            if (observer && serverObserver) {
                auto rot = rotation;
                rot.z += Flt32(f % 120) * 6.2831853f / 120;
                observer->setFlyMode(false, true, pos, rot);
                serverObserver->setFlyMode(false, true, pos, rot);
            }
            meter::Scope scope(meter::Frame);
            gfx::HardwareBatch batch;
            require(app.renderFrame(*surface, &batch), "frame preparation failed");
            if (app.lastScenePolygonCount())
                ++worldFrames;
#if defined(OPENT1_MEMORY_GPU)
            if (renderer) {
                gfx::GpuFrameConfig config;
                config.hazeColor = app.hardwareHazeColor();
                require(renderer->render(batch, 800, 600, config, 1) != nullptr,
                        "GPU frame failed");
                if ((f + 1) % 60 == 0 || f + 1 == frames) {
                    const auto c = renderer->textureCacheStats();
                    const auto b = renderer->resourceStats();
                    const auto d = renderer->submissionStats();
                    out << "{\"kind\":\"gpu\",\"frame\":" << f + 1 << ",\"textures\":" << c.textures
                        << ",\"physical_textures\":" << c.physicalTextures
                        << ",\"lightmap_pages\":" << c.lightMapPages
                        << ",\"lightmap_page_bytes\":" << c.lightMapPageBytes
                        << ",\"texture_arrays\":" << c.textureArrays
                        << ",\"texture_array_bytes\":" << c.textureArrayBytes
                        << ",\"allocated_texture_payload_bytes\":" << c.allocatedPayloadBytes
                        << ",\"rgba_bytes\":" << c.bytes << ",\"uploads\":" << c.uploads
                        << ",\"evictions\":" << c.evictions
                        << ",\"vertex_capacity_bytes\":" << b.vertexCapacity
                        << ",\"transfer_capacity_bytes\":" << b.uploadCapacity
                        << ",\"instance_capacity_bytes\":" << b.instanceCapacity
                        << ",\"instance_upload_bytes\":" << d.instanceUploadBytes
                        << ",\"retained_mesh_bytes\":" << b.meshBytes
                        << ",\"peak_retained_mesh_bytes\":" << b.peakMeshBytes
                        << ",\"mesh_uploads\":" << b.meshUploads
                        << ",\"draws\":" << d.draws << ",\"fence_wait_ms\":" << d.fenceWaitMs
                        << ",\"scene_polygons\":" << app.lastScenePolygonCount()
                        << ",\"scene_surfaces\":" << app.lastSceneSurfaceCount()
                        << ",\"scene_shapes\":" << app.lastSceneShapeCount();
                    videoMemory(out);
                    out << "}\n";
                }
            }
#endif
            if ((f + 1) % 60 == 0 || f + 1 == frames)
                snapshot(out, "render_warm");
        }
    }
    snapshot(out, "before_shutdown");
#if defined(OPENT1_MEMORY_GPU)
    renderer.reset();
    if (device) {
        SDL_DestroyGPUDevice(device);
        SDL_Quit();
    }
#endif
    app.shutdown();
    snapshot(out, "after_shutdown");
    require(frames == 0 || worldFrames > 0,
            "No world geometry rendered; GPU result is not a world measurement");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2,
                "Use: catalog ROOT OUTPUT DIRECTORY... | mission ROOT MISSION OUTPUT [--rpg] "
                "[--gpu] [--frames N] | self-test");
        const std::string_view mode = argv[1];
        if (mode == "self-test") {
            const auto baseline = meter::total.live.load();
            auto* p = ::operator new(123, std::align_val_t{256});
            require(reinterpret_cast<std::uintptr_t>(p) % 256 == 0, "alignment failed");
            require(meter::total.live.load() == baseline + 123, "allocation accounting failed");
            ::operator delete(p, std::align_val_t{256});
            require(meter::total.live.load() == baseline, "release accounting failed");
            std::cout << "allocation accounting passed\n";
            return 0;
        }
        require(argc >= 5, "missing arguments");
        const std::filesystem::path output = argv[mode == "catalog" ? 3 : 4];
        if (!output.parent_path().empty())
            std::filesystem::create_directories(output.parent_path());
        std::ofstream out(output);
        require(bool(out), "cannot open output");
        out << "{\"kind\":\"schema\",\"version\":1,\"pointer_bytes\":" << sizeof(void*)
            << ",\"hardware_vertex_bytes\":" << sizeof(gfx::HardwareVertex)
            << ",\"surface_triangle_bytes\":" << sizeof(world::StaticSurfaceTriangle)
            << ",\"allocation_header_bytes\":" << sizeof(meter::Header) << "}\n";
        if (mode == "catalog") {
            std::vector<std::string_view> directories;
            for (auto i : std::views::iota(4) | std::views::take_while([&](int i) {
                              return i < argc;
                          }))
                directories.emplace_back(argv[i]);
            catalog(argv[2], out, directories);
        } else if (mode == "mission") {
            bool rpg{}, gpu{};
            unsigned frames{};
            const auto arguments = std::span(argv, std::size_t(argc));
            for (auto argumentIt = std::ranges::next(arguments.begin(), 5, arguments.end());
                 argumentIt != arguments.end();
                 ++argumentIt) {
                const std::string_view option = *argumentIt;
                if (option == "--rpg")
                    rpg = true;
                else if (option == "--gpu")
                    gpu = true;
                else if (option == "--frames" && std::next(argumentIt) != arguments.end())
                    frames = unsigned(std::stoul(*++argumentIt));
                else
                    throw std::runtime_error("unknown mission option");
            }
            mission(argv[2], argv[3], output, out, rpg, frames, gpu);
        } else
            throw std::runtime_error("unknown mode");
    } catch (const std::exception& e) {
        std::cerr << "Memory audit: " << e.what() << '\n';
        return 1;
    }
}
