#pragma once

#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <vulkan/vulkan.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <vk_mem_alloc.h>

#include <iostream>
#include <sstream>
#include <vector>
#include <variant>
#include <chrono>
#include <thread>
#include <any>
#include <random>
#include <limits>

#include <hdvw/window.hpp>
#include <hdvw/instance.hpp>
#include <hdvw/surface.hpp>
#include <hdvw/device.hpp>
#include <hdvw/allocator.hpp>
#include <hdvw/queue.hpp>
#include <hdvw/commandpool.hpp>
#include <hdvw/commandbuffer.hpp>
#include <hdvw/swapchain.hpp>
#include <hdvw/renderpass.hpp>
#include <hdvw/framebuffer.hpp>
#include <hdvw/shader.hpp>
#include <hdvw/pipelinelayout.hpp>
#include <hdvw/pipeline.hpp>
#include <hdvw/semaphore.hpp>
#include <hdvw/fence.hpp>
#include <hdvw/vertex.hpp>
#include <hdvw/databuffer.hpp>
#include <hdvw/texture.hpp>
#include <hdvw/descriptorlayout.hpp>
#include <hdvw/descriptorpool.hpp>
#include <hdvw/descriptorset.hpp>

#include <engine/utils.hpp>
#include <engine/blas.hpp>
#include <engine/tlas.hpp>
#include <engine/sbt.hpp>
#include <engine/model.hpp>
#include <engine/camera.hpp>
#include <engine/saveimg.hpp>

#define MAX_FRAMES_IN_FLIGHT 3

struct params_t {
    uint32_t N = 1;
    std::string method = "ReSTIR";
    bool pseudoOffline = false;
    uint32_t frames = 6;
    bool capture = false;
    uint32_t tolerance = 0;
    uint32_t M = 4;
    bool accumulate;
    bool immediate = false;
    bool multiply = false;
};

struct UniformData {
    alignas(64) glm::mat4 viewInverse;
    alignas(64) glm::mat4 projInverse;
    alignas(16) glm::uvec4 state;
    alignas(4)  uint32_t frameIndex;
    alignas(4)  uint32_t N;
};

struct UniMotion {
    alignas(64) glm::mat4 forward;
    /* glm::dmat4 forward; */
    /* glm::dmat4 inverse; */
    /* glm::mat4 view; */
};

struct UniSizes {
    alignas(4) uint32_t meshesSize;
    alignas(4) uint32_t lightsSize;
    alignas(4) uint32_t M;
    alignas(4) uint32_t C;
};

struct PushWindowSize {
    alignas(4) uint32_t width;
    alignas(4) uint32_t height;
    alignas(4) uint32_t C;
};

struct UniCount {
    alignas(4) uint32_t count;
};

struct UniFrames {
    alignas(16) glm::uvec4 state;
    alignas(4)  uint32_t lightsSize;
    alignas(4)  uint32_t frames;
    alignas(16) glm::vec3 cameraPos;
};

class App {
    private:
        params_t params;
        UniSizes uniSizes{};

        bool framebufferResized = false;

        hd::Window window;
        hd::Instance instance;
        hd::Surface surface;
        hd::Device device;
        hd::Allocator allocator;
        hd::Queue graphicsQueue;
        hd::Queue presentQueue;
        hd::CommandPool graphicsPool;

        std::vector<hd::Semaphore> imageAvailable;
        std::vector<hd::Semaphore> renderFinished;
        std::vector<hd::Fence> inFlightFences;

        struct vram {
            using vram_vertices = hd::DataBuffer<hd::Vertex>;
            using vram_indices  = hd::DataBuffer<uint32_t>;
            using vram_texture  = hd::Texture;
            using vram_material = hd::DataBuffer<hd::Material>;

            std::vector<vram_vertices> vertices;
            std::vector<vram_indices>  indices;
            std::vector<vram_texture>  diffuse;
            std::vector<vram_material> materials;

            std::vector<vram_vertices> lightVertices;
            std::vector<vram_indices> lightIndices;
            hd::DataBuffer<hd::VRAM_Light> lights;

            hd::DataBuffer<UniSizes> uniSizes;

            std::vector<hd::BLAS> blases;
            hd::TLAS tlas;

            struct workImage {
                hd::Image image;
                hd::ImageView view;
            };

            struct storage {
                workImage frame;
                workImage summ;
            } storage;

            struct reservoir {
                workImage present;
                workImage vpos;
                workImage vnorm;
                workImage vmat;
                workImage past;
            } reservoir;

            hd::DataBuffer<UniformData> unibuffer;
            hd::DataBuffer<UniCount> uniCount;
            hd::DataBuffer<UniFrames> uniFrames;
            hd::DataBuffer<UniMotion> uniMotion;
        } vram;

        struct ram {
            hd::Image saveImage;
        } ram;

        hd::DescriptorLayout compLayout;
        hd::PipelineLayout compPipeLayout;

        hd::Pipeline summPipeline;
        hd::Pipeline spatialPipeline;

        hd::DescriptorLayout rayLayout;
        hd::PipelineLayout rayPipeLayout;

        hd::Pipeline rayPipeline;
        hd::SBT sbt;

        inline auto populateInitialVRAM(hd::Model scene, std::vector<hd::Light>& lights) {
            auto fillVRAMBuffer = [&]<class T>(std::vector<T> const& data, vk::BufferUsageFlags flags, VmaMemoryUsage usage = VMA_MEMORY_USAGE_GPU_ONLY) {
                return hd::conjure<T>({
                        .commandPool = graphicsPool,
                        .queue = graphicsQueue,
                        .allocator = allocator,
                        .device = device,
                        .data = data,
                        .usage = flags,
                        .memoryUsage = usage,
                        });
            };

            vram.vertices.reserve(scene->meshes.size());
            vram.indices.reserve(scene->meshes.size());
            vram.diffuse.reserve(scene->meshes.size());
            vram.blases.reserve(scene->meshes.size());

            std::vector<vk::AccelerationStructureInstanceKHR> instances;
            instances.reserve(scene->meshes.size());

            vk::TransformMatrixKHR vkDefault_transform = []() {
                glm::mat4 transform = glm::mat4(1.0f);
                const float *ptransform = (const float*)glm::value_ptr(transform);

                vk::TransformMatrixKHR vkTransform;
                memcpy(&vkTransform.matrix, ptransform, 3 * 4 * sizeof(float));

                return vkTransform;
            }();

            vk::AccelerationStructureInstanceKHR instanceInfo{};
            instanceInfo.transform = vkDefault_transform;
            instanceInfo.mask = 0xFF;
            instanceInfo.instanceShaderBindingTableRecordOffset = 0; // HitGroupId
            instanceInfo.setFlags(vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable);

            for (uint32_t iter = 0; iter < scene->meshes.size(); iter++) {
                vram.vertices.push_back(fillVRAMBuffer(scene->meshes[iter].vertices, vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer));
                vram.indices.push_back(fillVRAMBuffer(scene->meshes[iter].indices, vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer));
                vram.diffuse.push_back(scene->meshes[iter].diffuse[0]);
                vram.materials.push_back(fillVRAMBuffer(std::vector{scene->meshes[iter].material}, vk::BufferUsageFlagBits::eStorageBuffer));

                vram.blases.push_back(hd::conjure({
                        vram.vertices.back(),
                        vram.indices.back(),
                        graphicsPool,
                        graphicsQueue,
                        device,
                        allocator,
                        }));

                instanceInfo.instanceCustomIndex = iter; // InstanceId
                instanceInfo.accelerationStructureReference = vram.blases[iter]->address();
                instances.push_back(instanceInfo);
            }

            std::vector<hd::VRAM_Light> vram_lights;
            vram_lights.reserve(lights.size());

            vram.lightVertices.reserve(lights.size());
            vram.lightIndices.reserve(lights.size());

            for (uint32_t iter = 0; iter < lights.size(); iter++){
                auto lightPad = hd::Model_t::generateLightPad(lights[iter]);
                vram_lights.push_back(lightPad.props);

                vram.lightVertices.push_back(fillVRAMBuffer(lightPad.vertices, vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer));
                vram.lightIndices.push_back(fillVRAMBuffer(lightPad.indices, vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer));

                vram.blases.push_back(hd::conjure({
                        vram.lightVertices.back(),
                        vram.lightIndices.back(),
                        graphicsPool,
                        graphicsQueue,
                        device,
                        allocator,
                        }));

                instanceInfo.instanceCustomIndex = scene->meshes.size() + iter; // InstanceId
                instanceInfo.accelerationStructureReference = vram.blases[vram.blases.size() - 1]->address();
                instances.push_back(instanceInfo);
            }

            vram.lights = fillVRAMBuffer(vram_lights, vk::BufferUsageFlagBits::eStorageBuffer);

            auto instbuffer = fillVRAMBuffer(instances, vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR, VMA_MEMORY_USAGE_CPU_TO_GPU);

            vram.tlas = hd::conjure({
                    instbuffer,
                    graphicsPool,
                    graphicsQueue,
                    device,
                    allocator,
                    });

            instbuffer.reset();

            uniSizes.meshesSize = scene->meshes.size();
            uniSizes.lightsSize = lights.size();
            uniSizes.M = params.M;
            if (params.multiply)
                uniSizes.C = 1;
            else
                uniSizes.C = 0;

            auto allocVRAMUniBuffer = [&]<class T>(hd::DataBuffer<T>& buffer, T const& dataStruct) {
                buffer = hd::conjure<T>({
                        .commandPool = graphicsPool,
                        .queue = graphicsQueue,
                        .allocator = allocator,
                        .device = device,
                        .data = {dataStruct},
                        .usage = vk::BufferUsageFlagBits::eUniformBuffer,
                        .memoryUsage = VMA_MEMORY_USAGE_CPU_TO_GPU,
                        });
            };

            allocVRAMUniBuffer(vram.unibuffer, {});
            allocVRAMUniBuffer(vram.uniCount,  {});
            allocVRAMUniBuffer(vram.uniFrames, {});
            allocVRAMUniBuffer(vram.uniMotion, {});
            allocVRAMUniBuffer(vram.uniSizes,  uniSizes);
        }

        constexpr auto selectFeatures() {
            struct features {
                vk::PhysicalDeviceFeatures feats{};
                vk::PhysicalDeviceDescriptorIndexingFeatures desc_feats{};
                vk::PhysicalDeviceAccelerationStructureFeaturesKHR acc_feats{};
                vk::PhysicalDeviceRayTracingPipelineFeaturesKHR ray_feats{};
                vk::PhysicalDeviceBufferDeviceAddressFeatures buffer_feats{};
                vk::PhysicalDeviceScalarBlockLayoutFeatures scalar_feats{};
                vk::PhysicalDeviceFeatures2 feats2{};

                auto res() {
                    return feats2;
                }
            } features;

            features.feats.samplerAnisotropy = true;

            features.scalar_feats.scalarBlockLayout = true;

            features.desc_feats.runtimeDescriptorArray = true;
            features.desc_feats.shaderSampledImageArrayNonUniformIndexing = true;
            features.desc_feats.shaderStorageBufferArrayNonUniformIndexing = true;
            features.desc_feats.pNext = &features.scalar_feats;

            features.acc_feats.accelerationStructure = true;
            features.acc_feats.pNext = &features.desc_feats;

            features.ray_feats.rayTracingPipeline = true;
            features.ray_feats.pNext = &features.acc_feats;

            features.buffer_feats.bufferDeviceAddress = true;
            features.buffer_feats.pNext = &features.ray_feats;

            features.feats2.features = features.feats;
            features.feats2.pNext = &features.buffer_feats;

            return features;
        }

        inline auto populateRayPipeline() {
            hd::Shader raygenShader = hd::conjure({
                    .device = device,
                    .filename = "shaders/raygen.rgen.spv",
                    .stage = vk::ShaderStageFlagBits::eRaygenKHR,
                    });

            hd::Shader missShader = hd::conjure({
                    .device = device,
                    .filename = "shaders/miss.rmiss.spv",
                    .stage = vk::ShaderStageFlagBits::eMissKHR,
                    });

            hd::Shader shadowShader = hd::conjure({
                    .device = device,
                    .filename = "shaders/shadow.rmiss.spv",
                    .stage = vk::ShaderStageFlagBits::eMissKHR,
                    });

            std::stringstream rchit;
            rchit << "shaders/" << params.method << ".rchit.spv";
            hd::Shader closestHitShader = hd::conjure({
                    .device = device,
                    .filename = rchit.str().c_str(),
                    .stage = vk::ShaderStageFlagBits::eClosestHitKHR,
                    });

            vk::RayTracingShaderGroupCreateInfoKHR raygenGroupCI{};
            raygenGroupCI.type = vk::RayTracingShaderGroupTypeKHR::eGeneral;
            raygenGroupCI.generalShader = 0;
            raygenGroupCI.closestHitShader = VK_SHADER_UNUSED_KHR;
            raygenGroupCI.anyHitShader = VK_SHADER_UNUSED_KHR;
            raygenGroupCI.intersectionShader = VK_SHADER_UNUSED_KHR;

            vk::RayTracingShaderGroupCreateInfoKHR missGroupCI{};
            missGroupCI.type = vk::RayTracingShaderGroupTypeKHR::eGeneral;
            missGroupCI.generalShader = 1;
            missGroupCI.closestHitShader = VK_SHADER_UNUSED_KHR;
            missGroupCI.anyHitShader = VK_SHADER_UNUSED_KHR;
            missGroupCI.intersectionShader = VK_SHADER_UNUSED_KHR;

            vk::RayTracingShaderGroupCreateInfoKHR shadowGroupCI{};
            shadowGroupCI.type = vk::RayTracingShaderGroupTypeKHR::eGeneral;
            shadowGroupCI.generalShader = 2;
            shadowGroupCI.closestHitShader = VK_SHADER_UNUSED_KHR;
            shadowGroupCI.anyHitShader = VK_SHADER_UNUSED_KHR;
            shadowGroupCI.intersectionShader = VK_SHADER_UNUSED_KHR;

            vk::RayTracingShaderGroupCreateInfoKHR closesthitGroupCI{};
            closesthitGroupCI.type = vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup;
            closesthitGroupCI.generalShader = VK_SHADER_UNUSED_KHR;
            closesthitGroupCI.closestHitShader = 3;
            closesthitGroupCI.anyHitShader = VK_SHADER_UNUSED_KHR;
            closesthitGroupCI.intersectionShader = VK_SHADER_UNUSED_KHR;

            rayPipeline = hd::conjure({
                    .pipelineLayout = rayPipeLayout,
                    .device = device,
                    .shaderInfos = { raygenShader->info(), missShader->info(), shadowShader->info(), closestHitShader->info() },
                    .shaderGroups = { raygenGroupCI, missGroupCI, shadowGroupCI, closesthitGroupCI },
                    });
        }

        auto init() {
            window = hd::conjure({
                    .width = 1280,
                    .height = 704,
                    .title = "Ray traycing",
                    .cursorVisible = true,
                    .windowUser = this,
                    .framebufferSizeCallback = framebufferResizeCallback,
                    });

            auto instanceExtensions = window->getRequiredExtensions();
            instanceExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

            instance = hd::conjure({
                    .applicationName = "Raytraycing",
                    .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                    .engineName = "Hova's Engine",
                    .engineVersion = VK_MAKE_VERSION(2, 0, 0),
                    .apiVersion = VK_API_VERSION_1_2,
#ifndef NDEBUG
                    .validationLayers = { "VK_LAYER_KHRONOS_validation" },
#endif
                    .extensions = instanceExtensions,
                    });

            surface = hd::conjure({
                    .window = window,
                    .instance = instance,
                    });

            device = hd::conjure({
                    .instance = instance,
                    .surface = surface,
                    .extensions = { 
                        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                        VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
                        VK_KHR_SPIRV_1_4_EXTENSION_NAME,
                        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
                        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
                    },
                    .features = selectFeatures().res(),
#ifndef NDEBUG
                    .validationLayers = { "VK_LAYER_KHRONOS_validation" },
#endif
                    });

            allocator = hd::conjure({
                    .instance = instance,
                    .device = device,
                    });

            graphicsQueue = hd::conjure({
                    .device = device,
                    .type = hd::QueueType::eGraphics,
                    });

            presentQueue = hd::conjure({
                    .device = device,
                    .type = hd::QueueType::ePresent,
                    });

            graphicsPool = hd::conjure({
                    .device = device,
                    .family = hd::PoolFamily::eGraphics,
                    });

            imageAvailable.resize(MAX_FRAMES_IN_FLIGHT);
            renderFinished.resize(MAX_FRAMES_IN_FLIGHT);
            inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

            for (uint32_t iter = 0; iter < MAX_FRAMES_IN_FLIGHT; iter++) {
                imageAvailable[iter] = hd::Semaphore_t::conjure({.device = device});
                renderFinished[iter] = hd::Semaphore_t::conjure({.device = device});
                inFlightFences[iter] = hd::Fence_t::conjure({.device = device});
            }

            // BEGIN RAM
            /* std::vector<hd::Model> sceneModels; */
            /* sceneModels.reserve(2); */

            /* for (auto& model : sceneModels) { */
            /*     model = hd::conjure({ */
            /*             "models/scene.obj", */
            /*             [&](const char *filename){ */
            /*                 return hd::conjure({ */
            /*                         filename, */
            /*                         graphicsPool, */
            /*                         graphicsQueue, */
            /*                         allocator, */
            /*                         device, */
            /*                         }); }, */
            /*             }); */
            /* } */

            auto scene = hd::conjure({
                    "models/scene.obj",
                    [&](const char *filename){
                        return hd::conjure({
                                filename,
                                graphicsPool,
                                graphicsQueue,
                                allocator,
                                device,
                                }); },
                    params.multiply,
                    });

            auto lights = hd::Model_t::parseLights("models/scene.json", params.multiply);
            // END RAM

            populateInitialVRAM(scene, lights);

            auto bind = [](uint32_t idx, vk::DescriptorType type, vk::ShaderStageFlags stages, uint32_t count = 1) {
                vk::DescriptorSetLayoutBinding binding{};
                binding.binding = idx;
                binding.descriptorType = type;
                binding.descriptorCount = count;
                binding.stageFlags = stages;

                return binding;
            };

            compLayout = hd::conjure({
                    .device = device,
                    .bindings = { 
                        bind(0, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute),
                        bind(1, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute),
                        bind(2, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eCompute),
                        bind(3, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eCompute),
                        bind(4, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute),
                        bind(5, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute),
                        bind(6, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute),
                        bind(7, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eCompute),
                    },
                    });

            vk::PushConstantRange pushWindowSize{};
            pushWindowSize.stageFlags = vk::ShaderStageFlagBits::eCompute;
            pushWindowSize.offset = 0;
            pushWindowSize.size = sizeof(PushWindowSize);

            compPipeLayout = hd::conjure({
                    .device = device,
                    .descriptorLayouts = { compLayout->raw() },
                    .pushConstants = { pushWindowSize },
                    });

            hd::Shader summShader = hd::conjure({
                    .device = device,
                    .filename = "shaders/summ.comp.spv",
                    .stage = vk::ShaderStageFlagBits::eCompute,
                    });

            summPipeline = hd::conjure({
                .pipelineLayout = compPipeLayout,
                .device = device,
                .shaderInfo = summShader->info(),
            });

            hd::Shader spatialShader = hd::conjure({
                    .device = device,
                    .filename = "shaders/spatial.comp.spv",
                    .stage = vk::ShaderStageFlagBits::eCompute,
                    });

            spatialPipeline = hd::conjure({
                .pipelineLayout = compPipeLayout,
                .device = device,
                .shaderInfo = spatialShader->info(),
            });

            rayLayout = hd::conjure({
                    .device = device,
                    .bindings = { 
                        bind(0, vk::DescriptorType::eAccelerationStructureKHR, vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eClosestHitKHR),
                        bind(1, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eRaygenKHR),
                        bind(2, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eRaygenKHR),
                        bind(3, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eClosestHitKHR, vram.diffuse.size()),
                        bind(4, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eClosestHitKHR, vram.vertices.size()),
                        bind(5, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eClosestHitKHR, vram.indices.size()),
                        bind(6, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eClosestHitKHR, vram.materials.size()),
                        bind(7, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eClosestHitKHR),
                        bind(8, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eClosestHitKHR),
                        bind(9, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eClosestHitKHR),
                        bind(10, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eClosestHitKHR),
                        bind(11, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eClosestHitKHR),
                        bind(12, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eClosestHitKHR),
                        bind(13, vk::DescriptorType::eStorageImage, vk::ShaderStageFlagBits::eClosestHitKHR),
                        bind(14, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eClosestHitKHR),
                    },
                    });

            pushWindowSize.stageFlags = vk::ShaderStageFlagBits::eClosestHitKHR;
            rayPipeLayout = hd::conjure({
                    .device = device,
                    .descriptorLayouts = { rayLayout->raw() },
                    .pushConstants = { pushWindowSize },
                    });

            populateRayPipeline();

            sbt = hd::conjure({
                    .pipeline = rayPipeline,
                    .device = device,
                    .allocator = allocator,
                    .raygenCount = 1,
                    .missCount = 2,
                    .hitCount = 1,
                    });
        }

        hd::SwapChain swapChain;

        hd::DescriptorPool rayDescriptorPool;
        hd::DescriptorSet summDescriptorSet;
        hd::DescriptorSet spatialDescriptorSet;
        hd::DescriptorSet rayDescriptorSet;
        std::vector<hd::CommandBuffer> rayCmdBuffers;
        std::vector<hd::CommandBuffer> raySaveCmdBuffers;
        std::vector<hd::CommandBuffer> raySummCmdBuffers;
        std::vector<hd::Fence> inFlightImages;

        inline auto fillSpatialSet() {
            std::vector<std::variant<vk::DescriptorImageInfo, vk::DescriptorBufferInfo>> infos;
            infos.reserve(8);

            std::vector<vk::WriteDescriptorSet> writes;
            writes.reserve(8);

            auto write = [&](uint32_t binding, vk::DescriptorType type, uint32_t index = 0) {
                vk::WriteDescriptorSet writeSet{};
                writeSet.dstBinding = binding;
                writeSet.dstArrayElement = index;
                writeSet.descriptorType = type;
                writeSet.descriptorCount = 1;
                writeSet.dstSet = spatialDescriptorSet->raw();

                return writeSet;
            };

            auto fill = hd::make_overload(
                [&](uint32_t counter, vk::DescriptorImageInfo const& info, vk::DescriptorType type, uint32_t index = 0) {
                    infos.push_back(info);
                    auto aWrite = write(counter, type, index);
                    aWrite.setPImageInfo(&std::get<vk::DescriptorImageInfo>(infos.back()));
                    writes.push_back(aWrite);
                },

                [&](uint32_t counter, vk::DescriptorBufferInfo const& info, vk::DescriptorType type, uint32_t index = 0) {
                    infos.push_back(info);
                    auto aWrite = write(counter, type, index);
                    aWrite.setPBufferInfo(&std::get<vk::DescriptorBufferInfo>(infos.back()));
                    writes.push_back(aWrite);
                }
            );

            fill(0, vram.storage.frame.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(1, vram.reservoir.present.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(2, vram.uniFrames->writeInfo(), vk::DescriptorType::eUniformBuffer);
            fill(3, vram.lights->writeInfo(), vk::DescriptorType::eStorageBuffer);
            fill(4, vram.reservoir.vpos.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(5, vram.reservoir.vnorm.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(6, vram.reservoir.vmat.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(7, vram.reservoir.past.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);

            device->raw().updateDescriptorSets(writes, nullptr);
        }

        inline auto fillSummSet() {
            std::vector<std::variant<vk::DescriptorImageInfo, vk::DescriptorBufferInfo>> infos;
            infos.reserve(3);

            std::vector<vk::WriteDescriptorSet> writes;
            writes.reserve(3);

            auto write = [&](uint32_t binding, vk::DescriptorType type, uint32_t index = 0) {
                vk::WriteDescriptorSet writeSet{};
                writeSet.dstBinding = binding;
                writeSet.dstArrayElement = index;
                writeSet.descriptorType = type;
                writeSet.descriptorCount = 1;
                writeSet.dstSet = summDescriptorSet->raw();

                return writeSet;
            };

            auto fill = hd::make_overload(
                [&](uint32_t counter, vk::DescriptorImageInfo const& info, vk::DescriptorType type, uint32_t index = 0) {
                    infos.push_back(info);
                    auto aWrite = write(counter, type, index);
                    aWrite.setPImageInfo(&std::get<vk::DescriptorImageInfo>(infos.back()));
                    writes.push_back(aWrite);
                },

                [&](uint32_t counter, vk::DescriptorBufferInfo const& info, vk::DescriptorType type, uint32_t index = 0) {
                    infos.push_back(info);
                    auto aWrite = write(counter, type, index);
                    aWrite.setPBufferInfo(&std::get<vk::DescriptorBufferInfo>(infos.back()));
                    writes.push_back(aWrite);
                }
            );

            fill(0, vram.storage.frame.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(1, vram.storage.summ.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(2, vram.uniCount->writeInfo(), vk::DescriptorType::eUniformBuffer);

            device->raw().updateDescriptorSets(writes, nullptr);
        }

        inline auto fillRaySet() {
            std::vector<std::variant<vk::DescriptorImageInfo, vk::DescriptorBufferInfo, vk::WriteDescriptorSetAccelerationStructureKHR>> infos;
            infos.reserve(12 + 4 * vram.vertices.size());

            std::vector<vk::WriteDescriptorSet> writes;
            writes.reserve(12 + 4 * vram.vertices.size());

            auto write = [&](uint32_t binding, vk::DescriptorType type, uint32_t index = 0) {
                vk::WriteDescriptorSet writeSet{};
                writeSet.dstBinding = binding;
                writeSet.dstArrayElement = index;
                writeSet.descriptorType = type;
                writeSet.descriptorCount = 1;
                writeSet.dstSet = rayDescriptorSet->raw();

                return writeSet;
            };

            auto fill = hd::make_overload(
                [&](uint32_t counter, vk::WriteDescriptorSetAccelerationStructureKHR const& info, vk::DescriptorType type, uint32_t index = 0) {
                    infos.push_back(info);
                    auto aWrite = write(counter, type, index);
                    aWrite.setPNext(&std::get<vk::WriteDescriptorSetAccelerationStructureKHR>(infos.back()));
                    writes.push_back(aWrite);
                },

                [&](uint32_t counter, vk::DescriptorImageInfo const& info, vk::DescriptorType type, uint32_t index = 0) {
                    infos.push_back(info);
                    auto aWrite = write(counter, type, index);
                    aWrite.setPImageInfo(&std::get<vk::DescriptorImageInfo>(infos.back()));
                    writes.push_back(aWrite);
                },

                [&](uint32_t counter, vk::DescriptorBufferInfo const& info, vk::DescriptorType type, uint32_t index = 0) {
                    infos.push_back(info);
                    auto aWrite = write(counter, type, index);
                    aWrite.setPBufferInfo(&std::get<vk::DescriptorBufferInfo>(infos.back()));
                    writes.push_back(aWrite);
                }
            );

            fill(0, vram.tlas->writeInfo(), vk::DescriptorType::eAccelerationStructureKHR);
            fill(1, vram.storage.frame.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(2, vram.unibuffer->writeInfo(), vk::DescriptorType::eUniformBuffer);
            fill(7, vram.lights->writeInfo(), vk::DescriptorType::eStorageBuffer);
            fill(8, vram.uniSizes->writeInfo(), vk::DescriptorType::eUniformBuffer);
            fill(9, vram.reservoir.present.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(10, vram.reservoir.vpos.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(11, vram.reservoir.vnorm.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(12, vram.reservoir.vmat.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(13, vram.reservoir.past.view->writeInfo(vk::ImageLayout::eGeneral), vk::DescriptorType::eStorageImage);
            fill(14, vram.uniMotion->writeInfo(), vk::DescriptorType::eUniformBuffer);

            for (uint32_t iter = 0; iter < vram.vertices.size(); iter++) {
                fill(3, vram.diffuse[iter]->writeInfo(vk::ImageLayout::eShaderReadOnlyOptimal), vk::DescriptorType::eCombinedImageSampler, iter);
                fill(4, vram.vertices[iter]->writeInfo(), vk::DescriptorType::eStorageBuffer, iter);
                fill(5, vram.indices[iter]->writeInfo(), vk::DescriptorType::eStorageBuffer, iter);
                fill(6, vram.materials[iter]->writeInfo(), vk::DescriptorType::eStorageBuffer, iter);
            }

            device->raw().updateDescriptorSets(writes, nullptr);
        }

        inline auto fillReSTIRBuffer(hd::CommandBuffer buffer, uint32_t i) {
            vk::ImageSubresourceRange sRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };

            vk::ImageCopy copyRegion{};
            copyRegion.srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
            copyRegion.setSrcOffset({ 0, 0, 0 });
            copyRegion.dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
            copyRegion.setDstOffset({ 0, 0, 0 });
            copyRegion.setExtent({ swapChain->extent().width, swapChain->extent().height, 1 });

            const struct PushWindowSize dims = {
                vram.storage.frame.image->extent().width,
                vram.storage.frame.image->extent().height,
                uniSizes.C,
            };

            auto make = [&](auto const& image, vk::ImageLayout srcLayout, vk::ImageLayout dstLayout, 
                    vk::AccessFlags srcAccess, vk::AccessFlags dstAccess) {

                vk::ImageMemoryBarrier barrier = {};
                barrier.oldLayout = srcLayout;
                barrier.newLayout = dstLayout;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image->raw();
                barrier.subresourceRange = sRange;
                barrier.srcAccessMask = srcAccess;
                barrier.dstAccessMask = dstAccess;

                return barrier;
            };

            auto engage = [&](vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage, auto const& ...args) {
                buffer->raw().pipelineBarrier(srcStage, dstStage, vk::DependencyFlags{0}, nullptr, nullptr, { args... });
            };

            vk::ClearColorValue clearColor = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};

            ////////////////////////////////////////////////////////////////////////////////////////////////////////////

            buffer->begin();

            buffer->raw().bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, rayPipeline->raw());
            buffer->raw().bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, rayPipeLayout->raw(), 0, rayDescriptorSet->raw(), nullptr);

            vk::StridedDeviceAddressRegionKHR callableShaderSBTEntry{};

            buffer->raw().traceRaysKHR(
                    sbt->raygen().region,
                    sbt->miss().region,
                    sbt->hit().region,
                    callableShaderSBTEntry,
                    swapChain->extent().width,
                    swapChain->extent().height,
                    1
                    );

            engage(vk::PipelineStageFlagBits::eRayTracingShaderKHR, vk::PipelineStageFlagBits::eComputeShader,
                make(vram.reservoir.present.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eMemoryWrite,
                    vk::AccessFlagBits::eMemoryRead
                    ),
                make(vram.reservoir.vpos.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eMemoryWrite,
                    vk::AccessFlagBits::eMemoryRead
                    ),
                make(vram.reservoir.vnorm.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eMemoryWrite,
                    vk::AccessFlagBits::eMemoryRead
                    ),
                make(vram.reservoir.vmat.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eMemoryWrite,
                    vk::AccessFlagBits::eMemoryRead
                    ),
                make(vram.reservoir.past.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eMemoryRead,
                    vk::AccessFlagBits::eMemoryWrite
                    )
                );

            buffer->raw().pushConstants(compPipeLayout->raw(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(PushWindowSize), &dims);

            buffer->raw().bindDescriptorSets(vk::PipelineBindPoint::eCompute, compPipeLayout->raw(), 0, spatialDescriptorSet->raw(), nullptr);
            buffer->raw().bindPipeline(vk::PipelineBindPoint::eCompute, spatialPipeline->raw());
            buffer->raw().dispatch(uint32_t(ceil(swapChain->extent().width / 16.0f)), uint32_t(ceil(swapChain->extent().height / 16.0f)), 1);

            engage(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eRayTracingShaderKHR,
                make(vram.reservoir.vpos.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eMemoryRead,
                    vk::AccessFlags{0}
                    ),
                make(vram.reservoir.vnorm.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eMemoryRead,
                    vk::AccessFlags{0}
                    ),
                make(vram.reservoir.vmat.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eMemoryRead,
                    vk::AccessFlags{0}
                    ),
                make(vram.reservoir.past.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eMemoryWrite,
                    vk::AccessFlags{0}
                    )
                );

            engage(vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                make(swapChain->colorAttachment(i),
                    vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::AccessFlags{0},
                    vk::AccessFlagBits::eTransferWrite
                    ),
                make(vram.storage.frame.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eTransferSrcOptimal,
                    vk::AccessFlagBits::eMemoryWrite,
                    vk::AccessFlagBits::eTransferRead
                    )
                );

            engage(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                make(vram.reservoir.present.image,
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::AccessFlagBits::eTransferRead,
                    vk::AccessFlagBits::eTransferWrite
                    )
                );

            buffer->raw().clearColorImage(
                    vram.reservoir.present.image->raw(), 
                    vk::ImageLayout::eTransferDstOptimal, 
                    clearColor, 
                    sRange
                    );

            buffer->raw().copyImage(
                    vram.storage.frame.image->raw(), vk::ImageLayout::eTransferSrcOptimal, 
                    swapChain->colorAttachment(i)->raw(), vk::ImageLayout::eTransferDstOptimal, 
                    copyRegion
                    );

            engage(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eRayTracingShaderKHR,
                make(vram.reservoir.present.image,
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eTransferWrite,
                    vk::AccessFlags{0}
                    ),
                make(vram.storage.frame.image,
                    vk::ImageLayout::eTransferSrcOptimal,
                    vk::ImageLayout::eGeneral,
                    vk::AccessFlagBits::eTransferRead,
                    vk::AccessFlagBits::eMemoryWrite
                    )
            );

            engage(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                make(swapChain->colorAttachment(i),
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::ePresentSrcKHR,
                    vk::AccessFlagBits::eTransferWrite,
                    vk::AccessFlagBits::eMemoryRead
                    )
            );

            buffer->end();
        }

        inline auto fillEtraBuffer(hd::CommandBuffer buffer, uint32_t i) {
            vk::ImageSubresourceRange sRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };

            vk::ImageCopy copyRegion{};
            copyRegion.srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
            copyRegion.setSrcOffset({ 0, 0, 0 });
            copyRegion.dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
            copyRegion.setDstOffset({ 0, 0, 0 });
            copyRegion.setExtent({ swapChain->extent().width, swapChain->extent().height, 1 });

            const struct PushWindowSize dims = {
                vram.storage.frame.image->extent().width,
                vram.storage.frame.image->extent().height,
            };

            buffer->begin();

            buffer->raw().pushConstants(rayPipeLayout->raw(), vk::ShaderStageFlagBits::eClosestHitKHR, 0, sizeof(PushWindowSize), &dims);

            buffer->raw().bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, rayPipeline->raw());
            buffer->raw().bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, rayPipeLayout->raw(), 0, rayDescriptorSet->raw(), nullptr);

            vk::StridedDeviceAddressRegionKHR callableShaderSBTEntry{};

            buffer->raw().traceRaysKHR(
                    sbt->raygen().region,
                    sbt->miss().region,
                    sbt->hit().region,
                    callableShaderSBTEntry,
                    swapChain->extent().width,
                    swapChain->extent().height,
                    1
                    );

            buffer->transitionImageLayout({
                    swapChain->colorAttachment(i)->raw(),
                    vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal,
                    sRange,
                    });

            buffer->transitionImageLayout({
                    vram.storage.frame.image->raw(),
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eTransferSrcOptimal,
                    sRange,
                    });

            buffer->raw().copyImage(
                    vram.storage.frame.image->raw(), vk::ImageLayout::eTransferSrcOptimal, 
                    swapChain->colorAttachment(i)->raw(), vk::ImageLayout::eTransferDstOptimal, 
                    copyRegion
                    );

            buffer->transitionImageLayout({
                    swapChain->colorAttachment(i)->raw(),
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::ePresentSrcKHR,
                    sRange,
                    });

            buffer->transitionImageLayout({
                    vram.storage.frame.image->raw(),
                    vk::ImageLayout::eTransferSrcOptimal,
                    vk::ImageLayout::eGeneral,
                    sRange,
                    });

            buffer->end();
        }

        inline auto fillSummBuffer(hd::CommandBuffer buffer, uint32_t i) {
            const struct PushWindowSize dims = {
                vram.storage.frame.image->extent().width,
                vram.storage.frame.image->extent().height,
            };

            buffer->begin();

            buffer->raw().pushConstants(compPipeLayout->raw(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(PushWindowSize), &dims);

            buffer->raw().bindDescriptorSets(vk::PipelineBindPoint::eCompute, compPipeLayout->raw(), 0, summDescriptorSet->raw(), nullptr);
            buffer->raw().bindPipeline(vk::PipelineBindPoint::eCompute, summPipeline->raw());
            buffer->raw().dispatch(uint32_t(ceil(swapChain->extent().width / 16.0f)), uint32_t(ceil(swapChain->extent().height / 16.0f)), 1);

            buffer->end();
        }

        inline auto fillSaveBuffer(hd::CommandBuffer buffer, uint32_t i) {
            vk::ImageSubresourceRange sRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };

            auto source = (params.accumulate) ? vram.storage.summ.image : vram.storage.frame.image;

            buffer->begin();

            buffer->transitionImageLayout({
                    source->raw(),
                    vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eTransferSrcOptimal,
                    sRange,
                    });

            vk::ImageCopy copyRegion{};
            copyRegion.srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
            copyRegion.setSrcOffset({ 0, 0, 0 });
            copyRegion.dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
            copyRegion.setDstOffset({ 0, 0, 0 });
            copyRegion.setExtent({ source->extent().width, source->extent().height, 1 });

            if (params.accumulate) {
                const vk::Offset3D offsetStart = { 0, 0, 0 };
                const vk::Offset3D offsetEnd = {(int) swapChain->extent().width, (int) swapChain->extent().height, 1};

                vk::ImageBlit blitRegion{};
                blitRegion.srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
                blitRegion.setSrcOffsets({offsetStart, offsetEnd});
                blitRegion.dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
                blitRegion.setDstOffsets({offsetStart, offsetEnd});

                buffer->transitionImageLayout({
                        vram.storage.frame.image->raw(),
                        vk::ImageLayout::eGeneral,
                        vk::ImageLayout::eTransferDstOptimal,
                        sRange,
                        });

                buffer->raw().blitImage(
                        source->raw(), vk::ImageLayout::eTransferSrcOptimal,
                        vram.storage.frame.image->raw(), vk::ImageLayout::eTransferDstOptimal, 
                        blitRegion, vk::Filter::eNearest
                        );

                buffer->transitionImageLayout({
                        vram.storage.frame.image->raw(),
                        vk::ImageLayout::eTransferDstOptimal,
                        vk::ImageLayout::eTransferSrcOptimal,
                        sRange,
                        });

                buffer->raw().copyImage(
                        vram.storage.frame.image->raw(), vk::ImageLayout::eTransferSrcOptimal, 
                        ram.saveImage->raw(), vk::ImageLayout::eGeneral, 
                        copyRegion
                        );

                buffer->transitionImageLayout({
                        vram.storage.frame.image->raw(),
                        vk::ImageLayout::eTransferSrcOptimal,
                        vk::ImageLayout::eGeneral,
                        sRange,
                        });

                buffer->transitionImageLayout({
                        source->raw(),
                        vk::ImageLayout::eTransferSrcOptimal,
                        vk::ImageLayout::eGeneral,
                        sRange,
                        });
            } else {
                vk::ImageCopy copyRegion{};
                copyRegion.srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
                copyRegion.setSrcOffset({ 0, 0, 0 });
                copyRegion.dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
                copyRegion.setDstOffset({ 0, 0, 0 });
                copyRegion.setExtent({ source->extent().width, source->extent().height, 1 });

                buffer->raw().copyImage(
                        source->raw(), vk::ImageLayout::eTransferSrcOptimal, 
                        ram.saveImage->raw(), vk::ImageLayout::eGeneral, 
                        copyRegion
                        );

                buffer->transitionImageLayout({
                        source->raw(),
                        vk::ImageLayout::eTransferSrcOptimal,
                        vk::ImageLayout::eGeneral,
                        sRange,
                        });
            }

            buffer->end();
        }

        uint32_t globalFrameCount = 0;

        auto setup() {
            auto present = (params.immediate) ? vk::PresentModeKHR::eImmediate : vk::PresentModeKHR::eFifo;

            swapChain = hd::conjure(hd::SwapChainCreateInfo{
                    .window = window,
                    .surface = surface,
                    .allocator = allocator,
                    .device = device,
                    .presentMode = present,
                    });

            auto allocWorkImage = [&](vram::workImage& img, vk::Format format, vk::ImageUsageFlags flags, bool clear = false) {
                img.image = hd::conjure({
                        .allocator = allocator,
                        .extent = swapChain->extent(),
                        .format = format,
                        .imageUsage = vk::ImageUsageFlagBits::eStorage | flags,
                        .memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY,
                        });

                {
                    auto cmd = graphicsPool->singleTimeBegin();
                    cmd->transitionImageLayout({
                            .image = img.image,
                            .layout = vk::ImageLayout::eGeneral,
                            });

                    if (clear) {
                        vk::ClearColorValue color = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
                        vk::ImageSubresourceRange range = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

                        cmd->raw().clearColorImage(img.image->raw(), vk::ImageLayout::eGeneral, color, range);
                    }
                    graphicsPool->singleTimeEnd(cmd, graphicsQueue);
                }

                img.view = hd::conjure({
                        .image = img.image->raw(),
                        .device = device,
                        .format = img.image->format(),
                        .range = img.image->range(),
                        .type = vk::ImageViewType::e2D,
                        });
            };

            allocWorkImage(vram.storage.frame, swapChain->format(), vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst);
            allocWorkImage(vram.storage.summ, vk::Format::eR32G32B32A32Sfloat, vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, true);
            allocWorkImage(vram.reservoir.present, vk::Format::eR32G32B32A32Sfloat, vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, true);
            allocWorkImage(vram.reservoir.vnorm, vk::Format::eR32G32B32A32Sfloat, vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, true);
            allocWorkImage(vram.reservoir.vmat, vk::Format::eR32G32B32A32Sfloat, vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, true);
            allocWorkImage(vram.reservoir.vpos, vk::Format::eR32G32B32A32Sfloat, vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, true);
            allocWorkImage(vram.reservoir.past, vk::Format::eR32G32B32A32Sfloat, vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, true);

            ram.saveImage = hd::conjure({
                    .allocator = allocator,
                    .extent = swapChain->extent(),
                    .format = swapChain->format(),
                    .tiling = vk::ImageTiling::eLinear,
                    .imageUsage = vk::ImageUsageFlagBits::eTransferDst,
                    .memoryUsage = VMA_MEMORY_USAGE_CPU_ONLY,
                    });

            {
                auto cmd = graphicsPool->singleTimeBegin();
                cmd->transitionImageLayout({
                        .image = ram.saveImage,
                        .layout = vk::ImageLayout::eGeneral,
                        });
                graphicsPool->singleTimeEnd(cmd, graphicsQueue);
            }

            rayDescriptorPool = hd::conjure({
                    .device = device,
                    .layouts = {{rayLayout, 1}, {compLayout, 1}, {compLayout, 1}},
                    .instances = 1,
                   });

            summDescriptorSet = rayDescriptorPool->allocate(1, compLayout).at(0);
            spatialDescriptorSet = rayDescriptorPool->allocate(1, compLayout).at(0);
            rayDescriptorSet = rayDescriptorPool->allocate(1, rayLayout).at(0);

            fillSpatialSet();
            fillSummSet();
            fillRaySet();

            rayCmdBuffers = graphicsPool->allocate(swapChain->length());
            raySaveCmdBuffers = graphicsPool->allocate(swapChain->length());
            raySummCmdBuffers = graphicsPool->allocate(swapChain->length());
            for (uint32_t i = 0; i < swapChain->length(); i++) {
                if (params.method == "ReSTIR")
                    fillReSTIRBuffer(rayCmdBuffers[i], i);
                else
                    fillEtraBuffer(rayCmdBuffers[i], i);

                if (!params.capture)
                    continue;

                fillSummBuffer(raySummCmdBuffers[i], i);
                fillSaveBuffer(raySaveCmdBuffers[i], i);
            }

            inFlightImages.resize(swapChain->length(), nullptr);
        }

        void cleanupRender() {
            device->waitIdle();

            rayCmdBuffers.clear();
            ram.saveImage.reset();
            summDescriptorSet.reset();
            spatialDescriptorSet.reset();
            rayDescriptorSet.reset();
            vram.reservoir.present.image.reset();
            vram.reservoir.present.view.reset();
            vram.reservoir.vpos.image.reset();
            vram.reservoir.vpos.view.reset();
            vram.reservoir.vnorm.image.reset();
            vram.reservoir.vnorm.view.reset();
            vram.reservoir.vmat.image.reset();
            vram.reservoir.vmat.view.reset();
            vram.reservoir.past.image.reset();
            vram.reservoir.past.view.reset();
            vram.storage.frame.view.reset();
            vram.storage.frame.image.reset();
            swapChain.reset();

            device->updateSurfaceInfo();
        }

        void loop() {
            while (!window->shouldClose()) {
                window->pollEvents();
                update();
            }

            device->waitIdle();
        }

        void updateUnibuffer() {
            static auto startTime = std::chrono::high_resolution_clock::now();

            auto currentTime = std::chrono::high_resolution_clock::now();
            const double deltaTime = std::chrono::duration<double, std::chrono::seconds::period>(currentTime - startTime).count();

            startTime = currentTime;

            const float aspect = static_cast<float>(swapChain->extent().width) / static_cast<float>(swapChain->extent().height);
            const float cameraSpeed = 8.0 * deltaTime;
            const float cameraRotateSpeed = 3.0 * deltaTime;

            static glm::vec3 cameraPos;
            static float rotateXAngle, rotateYAngle, rotateZAngle;

            static bool static_if = true;
            if (static_if && params.multiply) {
                rotateXAngle = -0.600783;
                rotateYAngle = -0.918895;
                rotateZAngle = 3.14159;

                cameraPos = glm::vec3(6.53084, -38.4348, 10.4982);
                static_if = false;
            } else if (static_if) {
                rotateXAngle = 0.0f;
                rotateYAngle = -glm::half_pi<float>();
                rotateZAngle = glm::pi<float>();

                cameraPos = glm::vec3(13.5f, -3.0f, 0.0f);
                static_if = false;
            }

            static glm::vec3 cameraForward = glm::vec3(0.0f, 0.0f, 1.0f);
            static glm::vec3 cameraRight = glm::vec3(1.0f, 0.0f, 0.0f);
            static glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);

            const auto perspective = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 512.0f); // Only fov and aspect matter
            const auto view = [&]() {
                const auto translate = glm::translate(glm::mat4(1.0f), cameraPos);

                const glm::quat qPitch = glm::angleAxis(rotateXAngle, glm::vec3(1, 0, 0));
                const glm::quat qYaw = glm::angleAxis(rotateYAngle, glm::vec3(0, 1, 0));
                const glm::quat qRoll = glm::angleAxis(rotateZAngle,glm::vec3(0, 0, 1));

                glm::quat orientation = qPitch * qYaw * qRoll;
                orientation = glm::normalize(orientation);
                const glm::mat4 rotate = glm::mat4_cast(orientation);

                return rotate * translate;
            }();
            static auto oldView = view;

            static std::random_device rd;
            static std::default_random_engine generator(rd());
            static std::uniform_int_distribution<uint32_t> distribution(128, std::numeric_limits<uint32_t>::max() - swapChain->extent().width * swapChain->extent().height);

            const UniformData uniData {
                .viewInverse = glm::inverse(view),
                .projInverse = glm::inverse(perspective),
                .state = glm::uvec4(distribution(generator), distribution(generator), distribution(generator), distribution(generator)),
                .frameIndex = globalFrameCount,
                .N = params.N,
            };

            cameraForward = glm::normalize(glm::vec3(uniData.viewInverse[2]));
            cameraRight = glm::normalize(glm::vec3(uniData.viewInverse[0]));
            cameraUp = glm::normalize(glm::vec3(uniData.viewInverse[1]));

            if (glfwGetKey(window->raw(), GLFW_KEY_W) == GLFW_PRESS)
                cameraPos += cameraForward * cameraSpeed;
            if (glfwGetKey(window->raw(), GLFW_KEY_S) == GLFW_PRESS)
                cameraPos += cameraForward * -cameraSpeed;
            if (glfwGetKey(window->raw(), GLFW_KEY_A) == GLFW_PRESS)
                cameraPos += cameraRight * cameraSpeed;
            if (glfwGetKey(window->raw(), GLFW_KEY_D) == GLFW_PRESS)
                cameraPos += cameraRight * -cameraSpeed;
            if (glfwGetKey(window->raw(), GLFW_KEY_SPACE) == GLFW_PRESS)
                cameraPos += cameraUp * cameraSpeed;
            if (glfwGetKey(window->raw(), GLFW_KEY_BACKSPACE) == GLFW_PRESS)
                cameraPos += cameraUp * -cameraSpeed;

            if (glfwGetKey(window->raw(), GLFW_KEY_J) == GLFW_PRESS)
                rotateYAngle += -cameraRotateSpeed;
            if (glfwGetKey(window->raw(), GLFW_KEY_L) == GLFW_PRESS)
                rotateYAngle += cameraRotateSpeed;
            if (glfwGetKey(window->raw(), GLFW_KEY_I) == GLFW_PRESS)
                rotateXAngle += cameraRotateSpeed;
            if (glfwGetKey(window->raw(), GLFW_KEY_K) == GLFW_PRESS)
                rotateXAngle += -cameraRotateSpeed;


            const UniCount uniCount{
                .count = (globalFrameCount == params.frames) ? (params.frames - (params.tolerance)) : 1,
            };

            const UniFrames uniFrames{
                .state = uniData.state,
                /* .state = glm::uvec4(distribution(generator), distribution(generator), distribution(generator), distribution(generator)), */
                .lightsSize = uniSizes.lightsSize,
                .frames = globalFrameCount,
                .cameraPos = glm::vec3(uniData.viewInverse[3]),
            };
            /* std::cout << uniFrames.cameraPos[0] << ' ' << uniFrames.cameraPos[1] << ' ' << uniFrames.cameraPos[2] << std::endl; */

            const UniMotion uniMotion{
                .forward = glm::mat4(perspective) * glm::mat4(oldView),
                /* .forward = glm::dmat4(perspective) * glm::dmat4(oldView), */
                /* .inverse = glm::inverse(glm::dmat4(view)) * glm::inverse(glm::dmat4(perspective)), */
                /* .view = uniData.viewInverse, */
            };

            oldView = view;

            auto fillUniBuffer = []<class T>(auto const& uniBuffer, T const& dataStruct) {
                const auto data = uniBuffer->map();
                memcpy(data, &dataStruct, sizeof(T));
                uniBuffer->unmap();
            };

            fillUniBuffer(vram.unibuffer, uniData);
            fillUniBuffer(vram.uniCount,  uniCount);
            fillUniBuffer(vram.uniFrames, uniFrames);
            fillUniBuffer(vram.uniMotion, uniMotion);

            /* std::cout << cameraPos.x << ' ' << cameraPos.y << ' ' << cameraPos.z << std::endl; */
            /* std::cout << rotateXAngle << ' ' << rotateYAngle << ' ' << rotateZAngle << std::endl; */
        }

        uint32_t currentFrame = 0;
        void update() {
            static uint32_t screenshotFrame = -1;
            inFlightFences[currentFrame]->wait();

            if ((currentFrame == screenshotFrame) && params.capture) {
                if (params.pseudoOffline) {
                    hd::saveImg(ram.saveImage, device, allocator, params.method, params.N, params.tolerance, globalFrameCount - swapChain->length());
                    exit(0);
                }
                std::thread{hd::saveImg, ram.saveImage, device, allocator, params.method, params.N, params.tolerance, globalFrameCount - swapChain->length()}.detach();
                screenshotFrame = -1;
            }

            updateUnibuffer();

            uint32_t imageIndex;
            {
                auto result = device->acquireNextImage(swapChain->raw(), imageAvailable[currentFrame]->raw());

                if (result.result == vk::Result::eErrorOutOfDateKHR) {
                    cleanupRender();
                    setup();
                } else if (result.result != vk::Result::eSuccess && result.result != vk::Result::eSuboptimalKHR)
                    throw std::runtime_error("Failed to acquire the next image");
                imageIndex = result.value;
            }

            if (inFlightImages[imageIndex] != nullptr)
                inFlightImages[imageIndex]->wait();
            inFlightImages[imageIndex] = inFlightFences[currentFrame];

            {
                vk::Semaphore waitSemaphores[] = { imageAvailable[currentFrame]->raw() };
                vk::PipelineStageFlags waitStages[] = { vk::PipelineStageFlagBits::eColorAttachmentOutput };
                vk::Semaphore signalSemaphores[] = { renderFinished[currentFrame]->raw() };

                std::vector<vk::CommandBuffer> raw;

                if ((globalFrameCount == params.frames) && params.capture) {
                    raw.push_back(rayCmdBuffers[imageIndex]->raw());
                    raw.push_back(raySaveCmdBuffers[imageIndex]->raw());
                    screenshotFrame = currentFrame;
                } else if (params.accumulate && (globalFrameCount >= params.tolerance) && (globalFrameCount < params.frames) && params.capture) {
                    raw.push_back(rayCmdBuffers[imageIndex]->raw());
                    raw.push_back(raySummCmdBuffers[imageIndex]->raw());
                } else
                    raw.push_back(rayCmdBuffers[imageIndex]->raw());

                vk::SubmitInfo submitInfo = {};
                submitInfo.waitSemaphoreCount = 1;
                submitInfo.pWaitSemaphores = waitSemaphores;
                submitInfo.pWaitDstStageMask = waitStages;
                submitInfo.commandBufferCount = raw.size();
                submitInfo.pCommandBuffers = raw.data();
                submitInfo.signalSemaphoreCount = 1;
                submitInfo.pSignalSemaphores = signalSemaphores;

                inFlightFences[currentFrame]->reset();
                graphicsQueue->submit(submitInfo, inFlightFences[currentFrame]);
            }

            {
                vk::SwapchainKHR swapChains[] = { swapChain->raw() };

                vk::Semaphore waitSemaphores[] = { renderFinished[currentFrame]->raw() };

                vk::PresentInfoKHR presentInfo{};
                presentInfo.waitSemaphoreCount = 1;
                presentInfo.pWaitSemaphores = waitSemaphores;
                presentInfo.swapchainCount = 1;
                presentInfo.pSwapchains = swapChains;
                presentInfo.pImageIndices = &imageIndex;
                presentInfo.pResults = nullptr;

                vk::Result result = presentQueue->present(presentInfo);

                if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR || framebufferResized) {
                    framebufferResized = false;
                    cleanupRender();
                    setup();
                } else if (result != vk::Result::eSuccess)
                    throw std::runtime_error("Failed to present the image to the swapChain");
            }

            currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
            globalFrameCount++;
        }

        static void framebufferResizeCallback(GLFWwindow* window, int width, int height) {
            auto app = reinterpret_cast<App*>(glfwGetWindowUserPointer(window));
            app->framebufferResized = true;
        }

    public:
        App(params_t _params) : params(_params) {}

        void run() {
            init();
            setup();
            loop();
        }
};
