#include "sdl_gpu_material.hpp"
#include "gpu_shaders.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ranges>
#include <utility>

namespace opent1::gfx::detail {
namespace {
constexpr auto colorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr auto depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
constexpr UInt64 maxTransfer = 128U * 1024U * 1024U;

UInt64 align(UInt64 value, UInt64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool unit(Flt32 v) {
    return std::isfinite(v) && v >= 0 && v <= 1;
}

bool unit(Point3F v) {
    return unit(v.x) && unit(v.y) && unit(v.z);
}

bool equal(const HardwareTexture& a, const HardwareTexture& b) {
    return a.dynamic == b.dynamic && a.lightMap == b.lightMap && a.width == b.width &&
           a.height == b.height && a.rgba == b.rgba &&
           std::ranges::equal(a.mips, b.mips, [](const auto& x, const auto& y) {
               return x.width == y.width && x.height == y.height && x.rgba == y.rgba;
           });
}

SDL_GPUTexture* texture(SDL_GPUDevice* device,
                        Int32 width,
                        Int32 height,
                        bool depth = false,
                        bool target = false,
                        UInt32 levels = 1) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = depth ? depthFormat : colorFormat;
    info.usage = depth ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET : SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (target) {
        info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    }
    info.width = UInt32(width);
    info.height = UInt32(height);
    info.layer_count_or_depth = 1;
    info.num_levels = levels;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device, &info);
}

SDL_GPUShader* shader(SDL_GPUDevice* device, bool vertex) {
    SDL_GPUShaderCreateInfo info{};
    info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_uniform_buffers = 1;
    info.num_samplers = vertex ? 0 : 2;
    const auto formats = SDL_GetGPUShaderFormats(device);
    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.entrypoint = "main";
        info.code = vertex ? shaders::vert_spv : shaders::frag_spv;
        info.code_size = vertex ? sizeof(shaders::vert_spv) : sizeof(shaders::frag_spv);
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.entrypoint = "main";
        info.code = vertex ? shaders::vert_dxil : shaders::frag_dxil;
        info.code_size = vertex ? sizeof(shaders::vert_dxil) : sizeof(shaders::frag_dxil);
    } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        info.entrypoint = "main0";
        info.code = reinterpret_cast<const UInt8*>(vertex ? shaders::vert_msl : shaders::frag_msl);
        info.code_size = vertex ? sizeof(shaders::vert_msl) - 1 : sizeof(shaders::frag_msl) - 1;
    } else {
        SDL_SetError("No supported OpenT1 material shader format");
        return nullptr;
    }
    return SDL_CreateGPUShader(device, &info);
}

// Persistent targets and buffers are borrowed. New cache textures transfer
// ownership only after successful submission; errors cancel pending commands.
struct Resources {
    SDL_GPUDevice* device;
    SDL_GPUCommandBuffer* commands{};
    SDL_GPUTransferBuffer* transfer{};
    SDL_GPUBuffer* vertices{};
    SDL_GPUTexture *color{}, *depth{}, *corrected{};
    std::vector<SDL_GPUTexture*> textures;
    std::vector<SDL_GPUBuffer*> buffers{};
    std::vector<std::shared_ptr<LightMapSlot>> slots{};

    ~Resources() {
        if (commands) {
            SDL_CancelGPUCommandBuffer(commands);
        }
        std::ranges::for_each(textures, [&](auto* p) {
            SDL_ReleaseGPUTexture(device, p);
        });
        for (auto* buffer : buffers)
            SDL_ReleaseGPUBuffer(device, buffer);
    }
};

struct FragmentUniform {
    std::array<Flt32, 4> tint, haze, options, alpha;
    std::array<Flt32, 4> clip{};
    std::array<Flt32, 4> lightRect{1, 1, 0, 0};
    bool operator==(const FragmentUniform&) const = default;
};

static_assert(sizeof(FragmentUniform) == 96 && sizeof(HardwareVertex) == 48);

struct VertexUniform {
    std::array<Flt32, 4> frame;
    std::array<std::array<Flt32, 4>, 3> camera{};
    std::array<Flt32, 4> optics{}, clip{};
    bool operator==(const VertexUniform&) const = default;
};

static_assert(sizeof(VertexUniform) == 96);

struct DrawState {
    SDL_GPUGraphicsPipeline* pipeline;
    std::array<SDL_GPUTextureSamplerBinding, 2> samplers;
    VertexUniform frame;
    FragmentUniform material;
    SDL_GPUBuffer *vertices{}, *indices{};

    bool sameSamplers(const DrawState& other) const {
        for (std::size_t i = 0; i < samplers.size(); ++i) {
            if (samplers[i].texture != other.samplers[i].texture ||
                samplers[i].sampler != other.samplers[i].sampler) {
                return false;
            }
        }
        return true;
    }

    bool operator==(const DrawState& other) const {
        return pipeline == other.pipeline && sameSamplers(other) && frame == other.frame &&
               material == other.material && vertices == other.vertices && indices == other.indices;
    }
};

// Merge only consecutive triangle ranges, preserving primitive/blend order.
// The caller flushes at every material/haze/layer boundary. Bindings survive
// pipeline changes, but this encoder must be recreated for each render pass.
class DrawEncoder {
    SDL_GPUCommandBuffer* commands_;
    SDL_GPURenderPass* pass_;
    GpuSubmissionStats& stats_;
    std::optional<DrawState> bound_, pending_;
    UInt32 first_{}, count_{};

public:
    DrawEncoder(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, GpuSubmissionStats& stats)
        : commands_(commands), pass_(pass), stats_(stats) {}

    void flush() {
        if (!pending_) {
            return;
        }
        const auto& state = *pending_;
        if (!bound_ || bound_->pipeline != state.pipeline) {
            SDL_BindGPUGraphicsPipeline(pass_, state.pipeline);
            ++stats_.pipelineBinds;
        }
        if (!bound_ || !bound_->sameSamplers(state)) {
            SDL_BindGPUFragmentSamplers(
                pass_, 0, state.samplers.data(), UInt32(state.samplers.size()));
            ++stats_.samplerBinds;
        }
        if (!bound_ || bound_->frame != state.frame) {
            SDL_PushGPUVertexUniformData(commands_, 0, &state.frame, sizeof(state.frame));
            ++stats_.vertexUniformPushes;
        }
        if (!bound_ || bound_->material != state.material) {
            SDL_PushGPUFragmentUniformData(commands_, 0, &state.material, sizeof(state.material));
            ++stats_.fragmentUniformPushes;
        }
        if (!bound_ || bound_->vertices != state.vertices) {
            const SDL_GPUBufferBinding binding{state.vertices, 0};
            SDL_BindGPUVertexBuffers(pass_, 0, &binding, 1);
        }
        if (state.indices) {
            if (!bound_ || bound_->indices != state.indices) {
                const SDL_GPUBufferBinding binding{state.indices, 0};
                SDL_BindGPUIndexBuffer(pass_, &binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            }
            SDL_DrawGPUIndexedPrimitives(pass_, count_, 1, first_, 0, 0);
        } else
            SDL_DrawGPUPrimitives(pass_, count_, 1, first_, 0);
        ++stats_.draws;
        bound_ = state;
        pending_.reset();
    }

    void draw(const DrawState& state, UInt32 first, UInt32 count) {
        ++stats_.inputDraws;
        if (pending_ && first == first_ + count_ && *pending_ == state) {
            count_ += count;
            return;
        }
        flush();
        pending_ = state;
        first_ = first;
        count_ = count;
    }
};
}

GpuMaterialRenderer::~GpuMaterialRenderer() {
    clearTextureCache();
    for (const auto& [id, mesh] : meshes_) {
        (void)id;
        SDL_ReleaseGPUBuffer(device_, mesh.vertices);
        SDL_ReleaseGPUBuffer(device_, mesh.indices);
    }
    if (frameFence_) {
        SDL_ReleaseGPUFence(device_, frameFence_);
    }
    if (vertices_) {
        SDL_ReleaseGPUBuffer(device_, vertices_);
    }
    if (transfer_) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    }
    if (depth_) {
        SDL_ReleaseGPUTexture(device_, depth_);
    }
    for (const auto& target : targets_) {
        for (auto* p : {target.color, target.corrected}) {
            if (p) {
                SDL_ReleaseGPUTexture(device_, p);
            }
        }
    }
    std::ranges::for_each(pipelines_, [&](auto* p) {
        if (p) {
            SDL_ReleaseGPUGraphicsPipeline(device_, p);
        }
    });
    std::ranges::for_each(samplers_, [&](auto* p) {
        if (p) {
            SDL_ReleaseGPUSampler(device_, p);
        }
    });
}

void GpuMaterialRenderer::clearTextureCache() noexcept {
    for (auto& t : textures_) {
        if (!t.slot)
            SDL_ReleaseGPUTexture(device_, t.gpu);
    }
    stats_.evictions += textures_.size();
    textureIndex_.clear();
    textures_.clear();
    stats_.textures = stats_.bytes = 0;
    stats_.physicalTextures = stats_.lightMapPages = stats_.lightMapPageBytes = 0;
}

bool GpuMaterialRenderer::prepareResources(
    Int32 width, Int32 height, UInt32 vertexBytes, UInt32 transferBytes, bool gamma) {
    const auto ensureTarget = [&](SDL_GPUTexture*& target, bool depth) {
        if (target) {
            return true;
        }
        target = texture(device_, width, height, depth, !depth);
        if (!target) {
            return false;
        }
        ++resources_.targetAllocations;
        return true;
    };
    auto& target = targets_[nextTarget_];
    if (target.width != width || target.height != height) {
        for (auto* p : {target.color, target.corrected}) {
            if (p) {
                SDL_ReleaseGPUTexture(device_, p);
            }
        }
        target = {nullptr, nullptr, width, height};
    }
    if (depthWidth_ != width || depthHeight_ != height) {
        if (depth_) {
            SDL_ReleaseGPUTexture(device_, depth_);
        }
        depth_ = nullptr;
        depthWidth_ = width;
        depthHeight_ = height;
    }
    if (!ensureTarget(target.color, false) || !ensureTarget(depth_, true) ||
        (gamma && !ensureTarget(target.corrected, false))) {
        return false;
    }
    const auto capacity = [](UInt32 required) {
        UInt32 size = 4096;
        while (size < required) {
            size *= 2;
        }
        return size;
    };
    if (resources_.vertexCapacity < vertexBytes) {
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = capacity(vertexBytes);
        auto* next = SDL_CreateGPUBuffer(device_, &info);
        if (!next) {
            return false;
        }
        if (vertices_) {
            SDL_ReleaseGPUBuffer(device_, vertices_);
        }
        vertices_ = next;
        resources_.vertexCapacity = info.size;
        ++resources_.vertexAllocations;
    }
    if (resources_.uploadCapacity < transferBytes) {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = capacity(transferBytes);
        auto* next = SDL_CreateGPUTransferBuffer(device_, &info);
        if (!next) {
            return false;
        }
        if (transfer_) {
            SDL_ReleaseGPUTransferBuffer(device_, transfer_);
        }
        transfer_ = next;
        resources_.uploadCapacity = info.size;
        ++resources_.uploadAllocations;
    }
    return true;
}

std::unique_ptr<GpuMaterialRenderer> GpuMaterialRenderer::create(SDL_GPUDevice* device,
                                                                 std::size_t textureCacheEntries,
                                                                 std::size_t meshBytes,
                                                                 std::size_t textureBytes,
                                                                 std::size_t pageBytes) {
    if (!SDL_GPUTextureSupportsFormat(device,
                                      depthFormat,
                                      SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        SDL_SetError("Hardware materials require a D32 floating-point depth target");
        return {};
    }
    auto result = std::unique_ptr<GpuMaterialRenderer>(
        new GpuMaterialRenderer(device, textureCacheEntries, meshBytes, textureBytes, pageBytes));
    auto* vertex = shader(device, true);
    if (!vertex) {
        return {};
    }
    auto* fragment = shader(device, false);
    if (!fragment) {
        SDL_ReleaseGPUShader(device, vertex);
        return {};
    }
    const auto shadersDone = [&] {
        SDL_ReleaseGPUShader(device, vertex);
        SDL_ReleaseGPUShader(device, fragment);
    };
    SDL_GPUVertexBufferDescription buffer{};
    buffer.slot = 0;
    buffer.pitch = sizeof(HardwareVertex);
    buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    const std::array<SDL_GPUVertexAttribute, 7> attributes{
        {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, UInt32(offsetof(HardwareVertex, position))},
         {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, UInt32(offsetof(HardwareVertex, texture))},
         {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, UInt32(offsetof(HardwareVertex, color))},
         {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, UInt32(offsetof(HardwareVertex, haze))},
         {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, UInt32(offsetof(HardwareVertex, inverseDepth))},
         {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, UInt32(offsetof(HardwareVertex, lightMap))},
         {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, UInt32(offsetof(HardwareVertex, opacity))}}};
    const bool pipelines = std::ranges::all_of(std::views::iota(0U, 40U), [&](auto i) {
        const auto blend = HardwareBlend((i % 20) / 4);
        const auto depth = HardwareDepth(i % 4);
        SDL_GPUColorTargetDescription color{};
        color.format = colorFormat;
        color.blend_state.enable_blend = blend != HardwareBlend::Opaque;
        color.blend_state.src_color_blendfactor =
            blend == HardwareBlend::Subtractive || blend == HardwareBlend::Multiply
                ? SDL_GPU_BLENDFACTOR_ZERO
                : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color.blend_state.dst_color_blendfactor =
            blend == HardwareBlend::Multiply      ? SDL_GPU_BLENDFACTOR_SRC_COLOR
            : blend == HardwareBlend::Subtractive ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR
            : blend == HardwareBlend::Additive    ? SDL_GPU_BLENDFACTOR_ONE
                                                  : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.vertex_input_state = {&buffer, 1, attributes.data(), UInt32(attributes.size())};
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = i >= 20 ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
        info.rasterizer_state.enable_depth_clip = true;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.depth_stencil_state.enable_depth_test = depth != HardwareDepth::Disabled;
        info.depth_stencil_state.enable_depth_write =
            depth == HardwareDepth::TestAndWrite || depth == HardwareDepth::WriteOnly;
        info.depth_stencil_state.compare_op = depth == HardwareDepth::WriteOnly
                                                  ? SDL_GPU_COMPAREOP_ALWAYS
                                                  : SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = depthFormat;
        result->pipelines_[i] = SDL_CreateGPUGraphicsPipeline(device, &info);
        return result->pipelines_[i] != nullptr;
    });
    shadersDone();
    if (!pipelines) {
        return {};
    }
    if (!std::ranges::all_of(std::views::iota(0U, 24U), [&](auto i) {
            SDL_GPUSamplerCreateInfo info{};
            info.min_filter = i & 1 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
            info.mag_filter = i & 2 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
            info.mipmap_mode =
                i / 8 == 2 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            info.max_lod = i / 8 == 0 ? 0.f : Flt32(Bitmap::MaxDetailLevels - 1);
            info.address_mode_u = info.address_mode_v = info.address_mode_w =
                i & 4 ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
                      : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            result->samplers_[i] = SDL_CreateGPUSampler(device, &info);
            return result->samplers_[i] != nullptr;
        })) {
        return {};
    }
    return result;
}

SDL_GPUTexture* GpuMaterialRenderer::render(const HardwareBatch& batch,
                                            Int32 width,
                                            Int32 height,
                                            const GpuFrameConfig& config,
                                            Flt32 gamma) {
    const auto textures = batch.textures();
    if (textures.empty() || textures.size() != batch.textureHashes().size()) {
        SDL_SetError("Hardware batch must be prepared before rendering");
        return nullptr;
    }
    if (!unit(config.clearColor) || !unit(config.hazeColor) || !unit(config.clearAlpha)) {
        SDL_SetError("Invalid hardware frame color");
        return nullptr;
    }
    // Limit this surface to one hardware frame in flight, including hidden
    // windows which never acquire a swapchain to throttle submissions.
    double fenceWaitMs{};
    if (frameFence_) {
        const auto waitStart = std::chrono::steady_clock::now();
        if (!SDL_WaitForGPUFences(device_, true, &frameFence_, 1)) {
            return nullptr;
        }
        fenceWaitMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart)
                .count();
        SDL_ReleaseGPUFence(device_, std::exchange(frameFence_, nullptr));
    }

    struct Layout {
        UInt32 offset, pitch, texture, level, width, height;
        const std::byte* data;
    };

    Resources r{device_, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {}};
    std::unordered_map<UInt64, CachedMesh> pendingMeshes;
    std::vector<CachedMesh*> meshBindings;
    for (auto& [id, mesh] : meshes_) {
        (void)id;
        mesh.pinned = false;
    }
    std::size_t newMeshBytes{};
    for (const auto& instance : batch.meshes()) {
        const auto id = instance.mesh->revision();
        if (auto old = meshes_.find(id); old != meshes_.end()) {
            old->second.pinned = true;
            meshBindings.push_back(&old->second);
        } else {
            auto [next, inserted] = pendingMeshes.try_emplace(id);
            if (inserted) {
                auto& mesh = next->second;
                mesh.source = instance.mesh;
                mesh.bytes =
                    instance.mesh->vertices().size_bytes() + instance.mesh->indices().size_bytes();
                newMeshBytes += mesh.bytes;
                mesh.pinned = true;
                SDL_GPUBufferCreateInfo info{};
                info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
                info.size = UInt32(instance.mesh->vertices().size_bytes());
                mesh.vertices = SDL_CreateGPUBuffer(device_, &info);
                if (!mesh.vertices)
                    return nullptr;
                r.buffers.push_back(mesh.vertices);
                info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
                info.size = UInt32(instance.mesh->indices().size_bytes());
                mesh.indices = SDL_CreateGPUBuffer(device_, &info);
                if (!mesh.indices)
                    return nullptr;
                r.buffers.push_back(mesh.indices);
            }
            meshBindings.push_back(&next->second);
        }
    }
    for (auto it = meshes_.begin(); it != meshes_.end();) {
        if (!it->second.pinned && (it->second.source.use_count() == 1 ||
                                   resources_.meshBytes + newMeshBytes > meshCacheBytes_)) {
            SDL_ReleaseGPUBuffer(device_, it->second.vertices);
            SDL_ReleaseGPUBuffer(device_, it->second.indices);
            resources_.meshBytes -= it->second.bytes;
            it = meshes_.erase(it);
        } else
            ++it;
    }
    if (resources_.meshBytes + newMeshBytes > meshCacheBytes_) {
        SDL_SetError("Retained mesh budget exceeded");
        return nullptr;
    }

    std::vector<Layout> layout;
    layout.reserve(textures.size());
    // Six extra vertices support final-frame gamma, after all blending/haze.
    if (batch.vertices().size() > maxTransfer / sizeof(HardwareVertex) - 6) {
        SDL_SetError("Hardware vertex upload budget exceeded");
        return nullptr;
    }
    const auto vertexBytes = UInt32((batch.vertices().size() + 6) * sizeof(HardwareVertex));
    UInt64 transferBytes = vertexBytes;
    for (std::size_t i = 0; i < textures.size(); ++i) {
        const auto& t = textures[i];
        const auto add = [&](Int32 tw, Int32 th, UInt32 level, const auto& rgba) {
            const auto offset = align(transferBytes, 512),
                       pitch = align(UInt64(UInt32(tw)) * 4, 256);
            transferBytes = offset + pitch * UInt32(th);
            if (transferBytes > maxTransfer) {
                return false;
            }
            layout.push_back({UInt32(offset),
                              UInt32(pitch),
                              UInt32(i),
                              level,
                              UInt32(tw),
                              UInt32(th),
                              rgba.data()});
            return true;
        };
        if (!add(t.width, t.height, 0, t.rgba)) {
            SDL_SetError("Hardware texture upload budget exceeded");
            return nullptr;
        }
        for (std::size_t j = 0; j < t.mips.size(); ++j) {
            const auto& mip = t.mips[j];
            if (!add(mip.width, mip.height, UInt32(j + 1), mip.rgba)) {
                SDL_SetError("Hardware texture upload budget exceeded");
                return nullptr;
            }
        }
    }
    // The full frame's layout is validated even on cache hits. Cache residency
    // must never decide whether an otherwise identical batch exceeds its limits.
    std::ranges::for_each(textures_, [](auto& t) {
        t.pinned = false;
    });
    std::list<CachedTexture> pending;
    TextureIndex pendingIndex;
    std::vector<CachedTexture*> bindings;
    std::vector<bool> upload;
    std::size_t pendingBytes{};
    UInt64 hits{};
    for (std::size_t i = 0; i < textures.size(); ++i) {
        const auto& t = textures[i];
        const auto hash = batch.textureHashes()[i];
        auto bytes = t.rgba.size();
        for (const auto& mip : t.mips) {
            bytes += mip.rgba.size();
        }
        const auto find = [&](const auto& index, auto end) {
            const auto [first, last] = index.equal_range(hash);
            for (auto it = first; it != last; ++it) {
                const auto entry = it->second;
                if (&t == entry->source.get() || equal(t, *entry->source)) {
                    return entry;
                }
            }
            return end;
        };
        const auto found = find(textureIndex_, textures_.end());
        if (found != textures_.end()) {
            found->pinned = true;
            bindings.push_back(&*found);
            upload.push_back(false);
            ++hits;
            textures_.splice(textures_.begin(), textures_, found);
        } else {
            const auto duplicate = find(pendingIndex, pending.end());
            if (duplicate != pending.end()) {
                bindings.push_back(&*duplicate);
                upload.push_back(false);
                ++hits;
            } else {
                pending.push_back({batch.textureRefs()[i], hash, bytes, nullptr, true});
                pendingBytes += bytes;
                pendingIndex.emplace(hash, std::prev(pending.end()));
                bindings.push_back(&pending.back());
                upload.push_back(true);
            }
        }
    }
    UInt64 reuses{};
    const auto eraseCached = [&](auto entry) {
        const auto [first, last] = textureIndex_.equal_range(entry->hash);
        for (auto it = first; it != last; ++it) {
            if (it->second == entry) {
                textureIndex_.erase(it);
                break;
            }
        }
        stats_.bytes -= entry->bytes;
        --stats_.textures;
        textures_.erase(entry);
    };
    // All current-frame hits have been pinned before choosing recyclable slots.
    // A failed submission discards the detached allocation, keeping cache keys
    // honest even if upload preparation changed its contents.
    for (auto& next : pending) {
        if (next.source->lightMap) {
            if (!next.source->dynamic)
                continue;
            const auto victim =
                std::ranges::find_if(textures_.rbegin(), textures_.rend(), [&](const auto& old) {
                    return !old.pinned && old.slot && old.source->dynamic &&
                           old.source->width == next.source->width &&
                           old.source->height == next.source->height;
                });
            if (victim != textures_.rend()) {
                const auto entry = std::prev(victim.base());
                next.slot = entry->slot;
                next.gpu = entry->gpu;
                r.slots.push_back(next.slot);
                eraseCached(entry);
                ++reuses;
            }
            continue;
        }
        if (!next.source->dynamic) {
            continue;
        }
        const auto victim =
            std::ranges::find_if(textures_.rbegin(), textures_.rend(), [&](const auto& old) {
                return !old.pinned && !old.slot && !old.source->lightMap && old.source->dynamic &&
                       old.source->width == next.source->width &&
                       old.source->height == next.source->height &&
                       old.source->mips.size() == next.source->mips.size() &&
                       std::ranges::equal(
                           old.source->mips, next.source->mips, [](const auto& a, const auto& b) {
                               return a.width == b.width && a.height == b.height;
                           });
            });
        if (victim != textures_.rend()) {
            const auto entry = std::prev(victim.base());
            next.gpu = entry->gpu;
            r.textures.push_back(next.gpu);
            eraseCached(entry);
            ++reuses;
        }
    }
    // Protect every entry referenced by this frame while evicting older data.
    // Batch limits guarantee that the protected and pending set fits together.
    const auto frameEntries =
        pending.size() + std::size_t(std::ranges::count_if(textures_, [](const auto& t) {
            return t.pinned;
        }));
    const auto entryLimit = std::max(textureCacheEntries_, frameEntries);
    while (textures_.size() + pending.size() > entryLimit ||
           stats_.bytes + pendingBytes > textureCacheBytes_) {
        const auto victim =
            std::ranges::find_if(textures_.rbegin(), textures_.rend(), [](const auto& t) {
                return !t.pinned;
            });
        if (victim == textures_.rend()) {
            SDL_SetError("Hardware texture cache budget exceeded");
            return nullptr;
        }
        const auto oldest = std::prev(victim.base());
        if (!oldest->slot)
            SDL_ReleaseGPUTexture(device_, oldest->gpu);
        eraseCached(oldest);
        ++stats_.evictions;
    }
    for (auto& next : pending) {
        if (!next.slot && next.source->lightMap && next.source->mips.empty()) {
            next.slot = pages_.allocate(UInt32(next.source->width), UInt32(next.source->height));
            if (next.slot) {
                next.gpu = next.slot->page->texture;
                r.slots.push_back(next.slot);
            }
        }
    }
    transferBytes = vertexBytes;
    std::erase_if(layout, [&](const auto& t) {
        return !upload[t.texture];
    });
    for (auto& t : layout) {
        if (bindings[t.texture]->slot) {
            t.width += 2;
            t.height += 2;
            t.pitch = UInt32(align(UInt64(t.width) * 4, 256));
        }
        t.offset = UInt32(align(transferBytes, 512));
        transferBytes = t.offset + UInt64(t.pitch) * t.height;
        if (transferBytes > maxTransfer) {
            SDL_SetError("Lightmap page staging budget exceeded");
            return nullptr;
        }
    }

    struct MeshUpload {
        CachedMesh* mesh;
        UInt32 vertices, indices;
    };

    std::vector<MeshUpload> meshUploads;
    for (auto& [id, mesh] : pendingMeshes) {
        (void)id;
        transferBytes = align(transferBytes, 16);
        const auto vertexOffset = transferBytes;
        transferBytes += mesh.source->vertices().size_bytes();
        const auto indexOffset = transferBytes;
        transferBytes += mesh.source->indices().size_bytes();
        if (transferBytes > maxTransfer) {
            SDL_SetError("Retained mesh staging budget exceeded");
            return nullptr;
        }
        meshUploads.push_back({&mesh, UInt32(vertexOffset), UInt32(indexOffset)});
    }
    if (!prepareResources(width, height, vertexBytes, UInt32(transferBytes), gamma != 1)) {
        return nullptr;
    }
    r.color = targets_[nextTarget_].color;
    r.depth = depth_;
    r.corrected = gamma != 1 ? targets_[nextTarget_].corrected : nullptr;
    r.vertices = vertices_;
    r.transfer = transfer_;
    for (auto& t : pending) {
        if (t.gpu) {
            continue;
        }
        t.gpu = texture(device_,
                        t.source->width,
                        t.source->height,
                        false,
                        false,
                        UInt32(t.source->mips.size() + 1));
        if (!t.gpu) {
            return nullptr;
        }
        r.textures.push_back(t.gpu);
    }
    auto* mapped = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device_, r.transfer, false));
    if (!mapped) {
        return nullptr;
    }
    std::memset(mapped, 0, std::size_t(transferBytes));
    if (!batch.vertices().empty()) {
        std::memcpy(mapped, batch.vertices().data(), batch.vertices().size_bytes());
    }
    const Flt32 w = Flt32(width), h = Flt32(height);
    const std::array<HardwareVertex, 6> quad{{{{0, 0}, {0, 0}},
                                              {{w, 0}, {1, 0}},
                                              {{w, h}, {1, 1}},
                                              {{0, 0}, {0, 0}},
                                              {{w, h}, {1, 1}},
                                              {{0, h}, {0, 1}}}};
    std::memcpy(mapped + batch.vertices().size_bytes(), quad.data(), sizeof(quad));
    std::ranges::for_each(layout, [&](const auto& t) {
        std::ranges::for_each(std::views::iota(0U, t.height), [&](auto y) {
            if (bindings[t.texture]->slot) {
                const auto width = t.width - 2, height = t.height - 2;
                const auto sy = std::clamp(Int32(y) - 1, 0, Int32(height) - 1);
                for (UInt32 x = 0; x < t.width; ++x) {
                    const auto sx = std::clamp(Int32(x) - 1, 0, Int32(width) - 1);
                    std::memcpy(mapped + t.offset + std::size_t(y) * t.pitch + x * 4,
                                t.data + (std::size_t(sy) * width + UInt32(sx)) * 4,
                                4);
                }
            } else {
                std::memcpy(mapped + t.offset + std::size_t(y) * t.pitch,
                            t.data + std::size_t(y) * t.width * 4,
                            std::size_t(t.width) * 4);
            }
        });
    });
    for (const auto& uploadMesh : meshUploads) {
        std::memcpy(mapped + uploadMesh.vertices,
                    uploadMesh.mesh->source->vertices().data(),
                    uploadMesh.mesh->source->vertices().size_bytes());
        std::memcpy(mapped + uploadMesh.indices,
                    uploadMesh.mesh->source->indices().data(),
                    uploadMesh.mesh->source->indices().size_bytes());
    }
    SDL_UnmapGPUTransferBuffer(device_, r.transfer);
    r.commands = SDL_AcquireGPUCommandBuffer(device_);
    if (!r.commands) {
        return nullptr;
    }
    auto* copy = SDL_BeginGPUCopyPass(r.commands);
    if (!copy) {
        return nullptr;
    }
    SDL_GPUTransferBufferLocation source{r.transfer, 0};
    SDL_GPUBufferRegion destination{r.vertices, 0, vertexBytes};
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    for (const auto& uploadMesh : meshUploads) {
        SDL_GPUTransferBufferLocation from{r.transfer, uploadMesh.vertices};
        SDL_GPUBufferRegion to{
            uploadMesh.mesh->vertices, 0, UInt32(uploadMesh.mesh->source->vertices().size_bytes())};
        SDL_UploadToGPUBuffer(copy, &from, &to, false);
        from.offset = uploadMesh.indices;
        to = {uploadMesh.mesh->indices, 0, UInt32(uploadMesh.mesh->source->indices().size_bytes())};
        SDL_UploadToGPUBuffer(copy, &from, &to, false);
    }
    std::ranges::for_each(layout, [&](const auto& t) {
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = r.transfer;
        from.offset = t.offset;
        from.pixels_per_row = t.pitch / 4;
        from.rows_per_layer = t.height;
        SDL_GPUTextureRegion to{};
        to.texture = bindings[t.texture]->gpu;
        if (const auto& slot = bindings[t.texture]->slot) {
            to.x = slot->rect.x;
            to.y = slot->rect.y;
        }
        to.mip_level = t.level;
        to.w = t.width;
        to.h = t.height;
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, false);
    });
    SDL_EndGPUCopyPass(copy);
    const auto begin = [&](SDL_GPUTexture* color) {
        SDL_GPUColorTargetInfo target{};
        target.texture = color;
        target.clear_color = {
            config.clearColor.x, config.clearColor.y, config.clearColor.z, config.clearAlpha};
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = r.depth;
        depth.clear_depth = 0;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        return SDL_BeginGPURenderPass(r.commands, &target, 1, &depth);
    };
    auto* pass = begin(r.color);
    if (!pass) {
        return nullptr;
    }
    GpuSubmissionStats submission;
    submission.fenceWaitMs = fenceWaitMs;
    submission.vertexUploadBytes = vertexBytes;
    submission.meshUploadBytes = newMeshBytes;
    for (const auto& entry : layout)
        submission.textureUploadBytes += UInt64(entry.width) * entry.height * 4;
    DrawEncoder encoder(r.commands, pass, submission);
    const SDL_GPUBufferBinding binding{r.vertices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    const auto clip = config.clip ? intersect(*config.clip, {0, 0, width - 1, height - 1})
                                  : std::optional(RectI{0, 0, width - 1, height - 1});
    if (clip) {
        const SDL_Rect scissor{clip->left, clip->top, Int32(clip->width()), Int32(clip->height())};
        SDL_SetGPUScissor(pass, &scissor);
        UInt32 start{};
        for (const auto end : batch.layerEnds()) {
            const auto layer = batch.draws().subspan(start, end - start);
            std::ranges::for_each(layer, [&](const auto& draw) {
                const std::array<SDL_GPUTextureSamplerBinding, 2> samplers{
                    {{bindings[draw.texture]->gpu,
                      samplers_[std::size_t(draw.mipFilter) * 8 + (draw.wrap ? 4U : 0U) +
                                (draw.magnificationLinear ? 2U : 0U) + (draw.linear ? 1U : 0U)]},
                     {bindings[draw.lightMap]->gpu, samplers_[3]}}};
                VertexUniform frame{{w, h, draw.perspective ? 1.f : 0.f, draw.mesh ? 1.f : 0.f}};
                if (draw.mesh) {
                    const auto& transform = batch.meshes()[draw.mesh - 1].transform;
                    frame.camera = transform.camera;
                    frame.optics = transform.optics;
                    frame.clip = transform.clip;
                }
                FragmentUniform material{
                    {draw.color.x, draw.color.y, draw.color.z, draw.opacity},
                    {config.hazeColor.x, config.hazeColor.y, config.hazeColor.z, 0},
                    {1, 1, draw.lodBias, Flt32(draw.alphaSource)},
                    {draw.alphaCutoff,
                     draw.transparent ? 1.f : 0.f,
                     draw.deferredHaze ? 1.f : 0.f,
                     0}};
                if (draw.mesh)
                    material.clip = frame.clip;
                if (const auto& slot = bindings[draw.lightMap]->slot) {
                    constexpr Flt32 side = Flt32(LightMapPage::Side);
                    material.lightRect = {Flt32(slot->rect.width - 2) / side,
                                          Flt32(slot->rect.height - 2) / side,
                                          Flt32(slot->rect.x + 1) / side,
                                          Flt32(slot->rect.y + 1) / side};
                }
                auto* mesh = draw.mesh ? meshBindings[draw.mesh - 1] : nullptr;
                encoder.draw({pipelines_[(draw.mesh ? 20U : 0U) + std::size_t(draw.blend) * 4 +
                                         std::size_t(draw.depth)],
                              samplers,
                              frame,
                              material,
                              mesh ? mesh->vertices : r.vertices,
                              mesh ? mesh->indices : nullptr},
                             draw.firstVertex,
                             draw.vertexCount);
            });
            encoder.flush();
            for (const auto& draw : layer) {
                if (!draw.deferredHaze) {
                    continue;
                }
                const auto depth = draw.depth == HardwareDepth::Disabled ? HardwareDepth::Disabled
                                                                         : HardwareDepth::TestOnly;
                // The overlay shader uses only interpolated haze. Canonical
                // white bindings allow different base textures to share a run.
                const std::array<SDL_GPUTextureSamplerBinding, 2> samplers{
                    {{bindings[0]->gpu, samplers_[0]}, {bindings[0]->gpu, samplers_[3]}}};
                const VertexUniform frame{{w, h, draw.perspective ? 1.f : 0.f, 0}};
                const FragmentUniform material{
                    {1, 1, 1, 1},
                    {config.hazeColor.x, config.hazeColor.y, config.hazeColor.z, 0},
                    {1, 0, 0, 0},
                    {0, 0, 2, 0}};
                encoder.draw(
                    {pipelines_[std::size_t(HardwareBlend::Alpha) * 4 + std::size_t(depth)],
                     samplers,
                     frame,
                     material,
                     r.vertices,
                     nullptr},
                    draw.firstVertex,
                    draw.vertexCount);
            }
            encoder.flush();
            start = end;
        }
    }
    SDL_EndGPURenderPass(pass);
    if (r.corrected) {
        pass = begin(r.corrected);
        if (!pass) {
            return nullptr;
        }
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        const std::array<SDL_GPUTextureSamplerBinding, 2> samplers{
            {{r.color, samplers_[0]}, {bindings[0]->gpu, samplers_[3]}}};
        const VertexUniform frame{{w, h, 0, 0}};
        const FragmentUniform material{{1, 1, 1, 1}, {0, 0, 0, 0}, {1 / gamma, 0, 0, 0}, {}};
        DrawEncoder correction(r.commands, pass, submission);
        correction.draw({pipelines_[std::size_t(HardwareDepth::Disabled)],
                         samplers,
                         frame,
                         material,
                         r.vertices,
                         nullptr},
                        UInt32(batch.vertices().size()),
                        6);
        correction.flush();
        SDL_EndGPURenderPass(pass);
    }
    auto* commands = std::exchange(r.commands, nullptr);
    frameFence_ = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!frameFence_) {
        return nullptr;
    }
    submission_ = submission;
    resources_.meshAllocations += pendingMeshes.size() * 2;
    resources_.meshUploads += pendingMeshes.size();
    resources_.meshUploadBytes += newMeshBytes;
    resources_.meshBytes += newMeshBytes;
    resources_.peakMeshBytes = std::max(resources_.peakMeshBytes, resources_.meshBytes);
    meshes_.merge(pendingMeshes);
    resources_.meshes = meshes_.size();
    r.buffers.clear();
    stats_.uploads += pending.size();
    stats_.reuses += reuses;
    stats_.hits += hits;
    stats_.bytes += pendingBytes;
    textures_.splice(textures_.begin(), pending);
    textureIndex_.merge(pendingIndex);
    stats_.textures = textures_.size();
    stats_.lightMapPages = pages_.count();
    stats_.lightMapPageBytes = stats_.lightMapPages * LightMapPage::Bytes;
    stats_.physicalTextures =
        stats_.lightMapPages + std::size_t(std::ranges::count_if(textures_, [](const auto& entry) {
            return !entry.slot;
        }));
    r.textures.clear(); // ownership transfers to the cache only after submission
    nextTarget_ = 1 - nextTarget_;
    return r.corrected ? r.corrected : r.color;
}
} // namespace opent1::gfx::detail
