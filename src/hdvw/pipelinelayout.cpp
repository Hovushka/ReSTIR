#include <hdvw/pipelinelayout.hpp>
using namespace hd;

PipelineLayout_t::PipelineLayout_t(const PipelineLayoutCreateInfo& ci) {
    _device = ci.device->raw();

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.setLayoutCount = ci.descriptorLayouts.size();
    pipelineLayoutInfo.pSetLayouts = ci.descriptorLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = ci.pushConstants.size();
    pipelineLayoutInfo.pPushConstantRanges = ci.pushConstants.data();

    _pipelineLayout = _device.createPipelineLayout(pipelineLayoutInfo);
}

PipelineLayout_t::~PipelineLayout_t() {
    _device.destroy(_pipelineLayout);
}
