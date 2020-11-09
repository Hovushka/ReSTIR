#pragma once

#include <vulkan/vulkan.hpp>

#include <hdvw/buffer.hpp>
#include <hdvw/allocator.hpp>
#include <hdvw/pipeline.hpp>

#include <memory>
#include <vector>

namespace hd {
    struct SBTCreateInfo {
        Pipeline pipeline;
        Device device;
        Allocator allocator;
        uint32_t groupCount;
    };

    class SBT_t;
    typedef std::shared_ptr<SBT_t> SBT;

    class SBT_t {
        private:
            Buffer _buffer;
            vk::DeviceSize _shaderGroups;

        public:
            static SBT conjure(SBTCreateInfo ci) {
                return std::make_shared<SBT_t>(ci);
            }

            SBT_t(SBTCreateInfo ci) {
                auto device = ci.device->raw();
                auto rayProps = ci.device->_rayTracingProperties;
                
                _shaderGroups = ci.groupCount;
                const uint32_t sbtSize = rayProps.shaderGroupBaseAlignment * _shaderGroups;

                _buffer = Buffer_t::conjure({
                        .allocator = ci.allocator,
                        .size = sbtSize,
                        .bufferUsage = vk::BufferUsageFlagBits::eRayTracingKHR,
                        .memoryUsage = VMA_MEMORY_USAGE_CPU_TO_GPU,
                        });

                std::vector<uint8_t> shaderHandleStorage(sbtSize);

                if (device.getRayTracingShaderGroupHandlesKHR(
                        ci.pipeline->raw(), 0, _shaderGroups, sbtSize, shaderHandleStorage.data()) != vk::Result::eSuccess)
                    throw std::runtime_error("Couldn't retrieve shader groups' handles");

                auto data = static_cast<uint8_t*>(_buffer->map());
                for (uint32_t i = 0; i < _shaderGroups; i++) {
                    memcpy(data, shaderHandleStorage.data() + i * rayProps.shaderGroupHandleSize, rayProps.shaderGroupHandleSize);
                    data += rayProps.shaderGroupBaseAlignment;
                }
                _buffer->unmap();
            }

            vk::Buffer raw() {
                return _buffer->raw();
            }

            vk::DeviceSize size() {
                return _shaderGroups;
           }
    };
}
