#include "sdl_gpu_material.hpp"
#include "gpu_transfer_layout.hpp"
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
    info.num_samplers = vertex ? 0 : 3;
    info.num_storage_buffers = vertex ? 1 : 0;
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
    std::vector<std::shared_ptr<TextureArraySlot>> arrays{};

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
    std::array<Flt32, 4> imageArray{}; // layer, enabled, unused, unused
    bool operator==(const FragmentUniform&) const = default;
};

static_assert(sizeof(FragmentUniform) == 112 && sizeof(HardwareVertex) == 48);

// Terrain patches each own an instance, so their sampling coordinates can vary
// inside an instanced draw. Other mesh encodings retain per-material uniforms.
struct InstanceData {
    HardwareTransform transform;
    std::array<Flt32, 4> lightRect{1, 1, 0, 0};
    std::array<Flt32, 4> imageArray{};
};

static_assert(sizeof(InstanceData) == 288);

std::array<Flt32, 4> lightRect(const std::shared_ptr<LightMapSlot>& slot) {
    if (!slot)
        return {1, 1, 0, 0};
    constexpr Flt32 side = Flt32(LightMapPage::Side);
    return {Flt32(slot->rect.width - 2) / side,
            Flt32(slot->rect.height - 2) / side,
            Flt32(slot->rect.x + 1) / side,
            Flt32(slot->rect.y + 1) / side};
}

struct VertexUniform {
    std::array<Flt32, 4> frame;
    std::array<Flt32, 4> clip{};
    bool operator==(const VertexUniform&) const = default;
};

static_assert(sizeof(VertexUniform) == 32);

VertexUniform
vertexUniform(Flt32 w, Flt32 h, const HardwareDraw& draw, const HardwareBatch& batch) {
    VertexUniform result{{w, h, draw.perspective ? 1.f : 0.f, draw.mesh ? 2.f : 0.f}};
    if (draw.mesh) {
        const auto& t = batch.meshes()[draw.mesh - 1].transform;
        result.clip = t.clip;
    }
    return result;
}

struct DrawState {
    SDL_GPUGraphicsPipeline* pipeline;
    std::array<SDL_GPUTextureSamplerBinding, 3> samplers;
    VertexUniform frame;
    FragmentUniform material;
    SDL_GPUBuffer *vertices{}, *indices{};

    bool sameSamplers(const DrawState& other) const {
        for (auto i : std::views::iota(std::size_t{0}, samplers.size())) {
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
// The caller flushes at every composed layer boundary. Bindings survive
// pipeline changes, but this encoder must be recreated for each render pass.
class DrawEncoder {
    SDL_GPUCommandBuffer* commands_;
    SDL_GPURenderPass* pass_;
    GpuSubmissionStats& stats_;
    std::optional<DrawState> bound_, pending_;
    UInt32 first_{}, count_{}, firstInstance_{}, instances_{1};

public:
    DrawEncoder(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, GpuSubmissionStats& stats)
        : commands_(commands), pass_(pass), stats_(stats) {}

    void flush() {
        if (!pending_) {
            return;
        }
        auto state = *pending_;
        if (state.frame.frame[3] == 2)
            state.frame.clip[3] = Flt32(firstInstance_);
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
            SDL_DrawGPUIndexedPrimitives(pass_, count_, instances_, first_, 0, 0);
        } else
            SDL_DrawGPUPrimitives(pass_, count_, 1, first_, 0);
        ++stats_.draws;
        bound_ = state;
        pending_.reset();
    }

    void draw(const DrawState& state, UInt32 first, UInt32 count, UInt32 instance = 0) {
        ++stats_.inputDraws;
        if (pending_ && state.frame.frame[3] == 2 && first == first_ && count == count_ &&
            instance == firstInstance_ + instances_ && *pending_ == state) {
            ++instances_;
            return;
        }
        if (pending_ && instances_ == 1 && instance == firstInstance_ && first == first_ + count_ &&
            *pending_ == state) {
            count_ += count;
            return;
        }
        flush();
        pending_ = state;
        first_ = first;
        count_ = count;
        firstInstance_ = instance;
        instances_ = 1;
    }
};
}

GpuMaterialRenderer::~GpuMaterialRenderer() {
    clearTextureCache();
    if (whiteArray_)
        SDL_ReleaseGPUTexture(device_, whiteArray_);
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
    if (instances_)
        SDL_ReleaseGPUBuffer(device_, instances_);
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
        if (!t.slot && !t.array)
            SDL_ReleaseGPUTexture(device_, t.gpu);
    }
    stats_.evictions += textures_.size();
    textureIndex_.clear();
    textures_.clear();
    stats_.textures = stats_.bytes = 0;
    stats_.physicalTextures = stats_.lightMapPages = stats_.lightMapPageBytes = 0;
    stats_.textureArrays = stats_.textureArrayBytes = stats_.allocatedPayloadBytes = 0;
}

bool GpuMaterialRenderer::trimCaches() {
    if (frameFence_) {
        if (!SDL_WaitForGPUFences(device_, true, &frameFence_, 1))
            return false;
        SDL_ReleaseGPUFence(device_, std::exchange(frameFence_, nullptr));
    }
    clearTextureCache();
    for (const auto& [id, mesh] : meshes_) {
        (void)id;
        SDL_ReleaseGPUBuffer(device_, mesh.vertices);
        SDL_ReleaseGPUBuffer(device_, mesh.indices);
    }
    meshes_.clear();
    resources_.meshes = resources_.meshBytes = 0;
    if (vertices_)
        SDL_ReleaseGPUBuffer(device_, std::exchange(vertices_, nullptr));
    if (transfer_)
        SDL_ReleaseGPUTransferBuffer(device_, std::exchange(transfer_, nullptr));
    resources_.vertexCapacity = resources_.uploadCapacity = 0;
    if (instances_)
        SDL_ReleaseGPUBuffer(device_, std::exchange(instances_, nullptr));
    resources_.instanceCapacity = 0;
    return true;
}

bool GpuMaterialRenderer::prepareResources(
    Int32 width, Int32 height, UInt32 vertexBytes, UInt32 transferBytes, bool gamma) {
    if (transferBytes > stagingBytes_)
        return SDL_SetError("Hardware staging budget exceeded");
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
        info.size = std::min(capacity(transferBytes), stagingBytes_);
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
                                                                 std::size_t pageBytes,
                                                                 std::size_t arrayBytes,
                                                                 UInt32 stagingBytes) {
    if (stagingBytes < 4096 || stagingBytes > maxTransfer || textureBytes < 4 ||
        !textureCacheEntries) {
        SDL_SetError("Invalid material cache budget");
        return {};
    }
    if (!SDL_GPUTextureSupportsFormat(device,
                                      depthFormat,
                                      SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        SDL_SetError("Hardware materials require a D32 floating-point depth target");
        return {};
    }
    auto result = std::unique_ptr<GpuMaterialRenderer>(new GpuMaterialRenderer(
        device, textureCacheEntries, meshBytes, textureBytes, pageBytes, arrayBytes, stagingBytes));
    SDL_GPUTextureCreateInfo arrayInfo{};
    arrayInfo.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    arrayInfo.format = colorFormat;
    arrayInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    arrayInfo.width = arrayInfo.height = arrayInfo.layer_count_or_depth = arrayInfo.num_levels = 1;
    arrayInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    result->whiteArray_ = SDL_CreateGPUTexture(device, &arrayInfo);
    if (!result->whiteArray_)
        return {};
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
    std::unordered_map<UInt64, const HardwareMesh*> wanted;
    for (const auto& instance : batch.meshes())
        wanted.emplace(instance.mesh->revision(), instance.mesh.get());
    std::size_t newMeshBytes{};
    for (const auto& [id, mesh] : wanted) {
        if (!meshes_.contains(id))
            newMeshBytes += mesh->vertices().size_bytes() + mesh->indices().size_bytes();
    }
    for (auto& [id, mesh] : meshes_) {
        mesh.pinned = wanted.contains(id);
    }
    for (auto it = meshes_.begin(); it != meshes_.end();) {
        if (!it->second.pinned && resources_.meshBytes + newMeshBytes > meshCacheBytes_) {
            SDL_ReleaseGPUBuffer(device_, it->second.vertices);
            SDL_ReleaseGPUBuffer(device_, it->second.indices);
            resources_.meshBytes -= it->second.bytes;
            it = meshes_.erase(it);
        } else
            ++it;
    }
    resources_.meshes = meshes_.size();
    if (newMeshBytes > meshCacheBytes_ || resources_.meshBytes > meshCacheBytes_ - newMeshBytes) {
        SDL_SetError("Retained mesh budget exceeded");
        return nullptr;
    }
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
    const auto rowAlignment = textureRowAlignment(device_);
    UInt64 transferBytes = vertexBytes;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < textures.size();
                  })) {
        const auto& t = textures[i];
        const auto add = [&](Int32 tw, Int32 th, UInt32 level, const auto& rgba) {
            const auto offset = align(transferBytes, 512),
                       pitch = rgbaTransferPitch(UInt32(tw), rowAlignment);
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
        for (auto j : std::views::iota(std::size_t{0}, t.mips.size())) {
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
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < textures.size();
                  })) {
        const auto& t = textures[i];
        const auto hash = batch.textureHashes()[i];
        auto bytes = t.rgba.size();
        for (const auto& mip : t.mips) {
            bytes += mip.rgba.size();
        }
        const auto find = [&](const auto& index, auto end) {
            const auto [first, last] = index.equal_range(hash);
            for (const auto& [key, entry] : std::ranges::subrange(first, last)) {
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
        const auto found =
            std::ranges::find(std::ranges::subrange(first, last), entry, [](const auto& item) {
                return item.second;
            });
        if (found != last) {
            textureIndex_.erase(found);
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
                return !old.pinned && !old.slot && !old.array && !old.source->lightMap &&
                       old.source->dynamic && old.source->width == next.source->width &&
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
        if (!oldest->slot && !oldest->array)
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
        if (!next.source->lightMap && !next.source->dynamic &&
            next.source != batch.textureRefs()[0]) {
            next.array = arrays_.allocate(*next.source);
            if (next.array) {
                next.gpu = next.array->page->texture;
                r.arrays.push_back(next.array);
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
            t.pitch = UInt32(rgbaTransferPitch(t.width, rowAlignment));
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
    const auto instanceOffset = align(transferBytes, 16);
    const auto instanceBytes =
        std::max<std::size_t>(1, batch.meshes().size()) * sizeof(InstanceData);
    transferBytes = instanceOffset + instanceBytes;
    if (transferBytes > stagingBytes_)
        return SDL_SetError("Hardware instance staging budget exceeded"), nullptr;
    if (resources_.instanceCapacity < instanceBytes) {
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        UInt32 capacity = 4096;
        while (capacity < instanceBytes)
            capacity *= 2;
        info.size = std::min(capacity, stagingBytes_);
        auto* next = SDL_CreateGPUBuffer(device_, &info);
        if (!next)
            return nullptr;
        if (instances_)
            SDL_ReleaseGPUBuffer(device_, instances_);
        instances_ = next;
        resources_.instanceCapacity = info.size;
        resources_.peakInstanceCapacity = std::max(resources_.peakInstanceCapacity, info.size);
        ++resources_.instanceAllocations;
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
    for (auto i : std::views::iota(std::size_t{0}, batch.meshes().size())) {
        if (batch.meshes()[i].transform.geometry[2] == 1)
            continue; // Filled with the patch's sampling data below.
        const InstanceData instance{batch.meshes()[i].transform};
        std::memcpy(
            mapped + instanceOffset + i * sizeof(InstanceData), &instance, sizeof(instance));
    }
    for (const auto& draw : batch.draws()) {
        if (!draw.mesh || batch.meshes()[draw.mesh - 1].transform.geometry[2] != 1)
            continue;
        // HardwareBatch publishes one instance per native patch, including
        // composed layers; no other material can overwrite this patch's data.
        const auto* image = bindings[draw.texture];
        InstanceData instance{batch.meshes()[draw.mesh - 1].transform,
                              lightRect(bindings[draw.lightMap]->slot)};
        if (image->array)
            instance.imageArray = {Flt32(image->array->layer), 1, 0, 0};
        std::memcpy(mapped + instanceOffset + (draw.mesh - 1) * sizeof(InstanceData),
                    &instance,
                    sizeof(instance));
    }
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
                for (auto x : std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 x) {
                                  return x < t.width;
                              })) {
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
    source.offset = UInt32(instanceOffset);
    destination = {instances_, 0, UInt32(instanceBytes)};
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
        if (const auto& slot = bindings[t.texture]->array)
            to.layer = slot->layer;
        to.mip_level = t.level;
        to.w = t.width;
        to.h = t.height;
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, false);
        if (t.texture == 0 && t.level == 0) {
            to.texture = whiteArray_;
            SDL_UploadToGPUTexture(copy, &from, &to, false);
        }
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
    submission.instanceUploadBytes = instanceBytes;
    for (const auto& entry : layout)
        submission.textureUploadBytes += UInt64(entry.width) * entry.height * 4;
    DrawEncoder encoder(r.commands, pass, submission);
    const SDL_GPUBufferBinding binding{r.vertices, 0};
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
    SDL_BindGPUVertexStorageBuffers(pass, 0, &instances_, 1);
    const auto clip = config.clip ? intersect(*config.clip, {0, 0, width - 1, height - 1})
                                  : std::optional(RectI{0, 0, width - 1, height - 1});
    if (clip) {
        const SDL_Rect scissor{clip->left, clip->top, Int32(clip->width()), Int32(clip->height())};
        SDL_SetGPUScissor(pass, &scissor);
        UInt32 start{};
        for (const auto end : batch.layerEnds()) {
            const auto layer = batch.draws().subspan(start, end - start);
            std::ranges::for_each(layer, [&](const auto& draw) {
                const auto* image = bindings[draw.texture];
                auto* imageSampler =
                    samplers_[std::size_t(draw.mipFilter) * 8 + (draw.wrap ? 4U : 0U) +
                              (draw.magnificationLinear ? 2U : 0U) + (draw.linear ? 1U : 0U)];
                const std::array<SDL_GPUTextureSamplerBinding, 3> samplers{
                    {{image->array ? bindings[0]->gpu : image->gpu, imageSampler},
                     {bindings[draw.lightMap]->gpu, samplers_[3]},
                     {image->array ? image->gpu : whiteArray_, imageSampler}}};
                const auto frame = vertexUniform(w, h, draw, batch);
                // Fog the source before blending. Fully fogged effects must
                // approach their blend's neutral color, without adding fog light.
                auto fog = config.hazeColor;
                if (draw.blend == HardwareBlend::Additive ||
                    draw.blend == HardwareBlend::Subtractive)
                    fog = {0, 0, 0};
                else if (draw.blend == HardwareBlend::Multiply)
                    fog = {1, 1, 1};
                FragmentUniform material{{draw.color.x, draw.color.y, draw.color.z, draw.opacity},
                                         {fog.x, fog.y, fog.z, 0},
                                         {1, 1, draw.lodBias, Flt32(draw.alphaSource)},
                                         {draw.alphaCutoff, draw.transparent ? 1.f : 0.f, 0, 0}};
                if (draw.mesh)
                    material.clip = frame.clip;
                if (!draw.mesh || batch.meshes()[draw.mesh - 1].transform.geometry[2] != 1) {
                    if (image->array)
                        material.imageArray = {Flt32(image->array->layer), 1, 0, 0};
                    material.lightRect = lightRect(bindings[draw.lightMap]->slot);
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
                             draw.vertexCount,
                             frame.frame[3] == 2 ? draw.mesh - 1 : 0);
            });
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
        SDL_BindGPUVertexStorageBuffers(pass, 0, &instances_, 1);
        const std::array<SDL_GPUTextureSamplerBinding, 3> samplers{
            {{r.color, samplers_[0]},
             {bindings[0]->gpu, samplers_[3]},
             {whiteArray_, samplers_[0]}}};
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
    stats_.textureArrays = arrays_.count();
    stats_.textureArrayBytes = arrays_.bytes();
    stats_.physicalTextures = stats_.lightMapPages + stats_.textureArrays +
                              std::size_t(std::ranges::count_if(textures_, [](const auto& entry) {
                                  return !entry.slot && !entry.array;
                              }));
    stats_.allocatedPayloadBytes = stats_.lightMapPageBytes + stats_.textureArrayBytes;
    for (const auto& entry : textures_)
        if (!entry.slot && !entry.array)
            stats_.allocatedPayloadBytes += entry.bytes;
    r.textures.clear(); // ownership transfers to the cache only after submission
    nextTarget_ = 1 - nextTarget_;
    return r.corrected ? r.corrected : r.color;
}
} // namespace opent1::gfx::detail
