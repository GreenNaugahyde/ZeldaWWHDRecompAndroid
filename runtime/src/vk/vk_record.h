#pragma once

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gfx::rec {

struct Stream {
    std::vector<uint8_t> data;
};

bool enabled();
VkCommandBuffer virtual_command_buffer();
void begin();
Stream finish();
void recycle(Stream&& stream);
void replay(const Stream& stream, VkCommandBuffer cmd);

void CmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
void CmdEndRenderPass(VkCommandBuffer);
void CmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
void CmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                           const VkDescriptorSet*, uint32_t, const uint32_t*);
void CmdBindVertexBuffers(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*);
void CmdBindIndexBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
void CmdSetViewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
void CmdSetScissor(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*);
void CmdSetBlendConstants(VkCommandBuffer, const float[4]);
void CmdSetDepthBias(VkCommandBuffer, float, float, float);
void CmdSetStencilCompareMask(VkCommandBuffer, VkStencilFaceFlags, uint32_t);
void CmdSetStencilReference(VkCommandBuffer, VkStencilFaceFlags, uint32_t);
void CmdSetStencilWriteMask(VkCommandBuffer, VkStencilFaceFlags, uint32_t);
void CmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*);
void CmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
void CmdDrawIndexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
void CmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
void CmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                        uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
                        uint32_t, const VkImageMemoryBarrier*);
void CmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy*);
void CmdCopyImage2(VkCommandBuffer, const VkCopyImageInfo2*);
void CmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter);
void CmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*);
void CmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*);
void CmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t,
                        const VkImageSubresourceRange*);
void CmdClearDepthStencilImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue*, uint32_t,
                               const VkImageSubresourceRange*);
void CmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t);
void CmdWriteTimestamp(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t);

}  // namespace gfx::rec

#ifndef WWHD_VK_RECORD_IMPLEMENTATION
#define vkCmdBeginRenderPass gfx::rec::CmdBeginRenderPass
#define vkCmdEndRenderPass gfx::rec::CmdEndRenderPass
#define vkCmdBindPipeline gfx::rec::CmdBindPipeline
#define vkCmdBindDescriptorSets gfx::rec::CmdBindDescriptorSets
#define vkCmdBindVertexBuffers gfx::rec::CmdBindVertexBuffers
#define vkCmdBindIndexBuffer gfx::rec::CmdBindIndexBuffer
#define vkCmdSetViewport gfx::rec::CmdSetViewport
#define vkCmdSetScissor gfx::rec::CmdSetScissor
#define vkCmdSetBlendConstants gfx::rec::CmdSetBlendConstants
#define vkCmdSetDepthBias gfx::rec::CmdSetDepthBias
#define vkCmdSetStencilCompareMask gfx::rec::CmdSetStencilCompareMask
#define vkCmdSetStencilReference gfx::rec::CmdSetStencilReference
#define vkCmdSetStencilWriteMask gfx::rec::CmdSetStencilWriteMask
#define vkCmdPushConstants gfx::rec::CmdPushConstants
#define vkCmdDraw gfx::rec::CmdDraw
#define vkCmdDrawIndexed gfx::rec::CmdDrawIndexed
#define vkCmdDispatch gfx::rec::CmdDispatch
#define vkCmdPipelineBarrier gfx::rec::CmdPipelineBarrier
#define vkCmdCopyImage gfx::rec::CmdCopyImage
#define vkCmdCopyImage2 gfx::rec::CmdCopyImage2
#define vkCmdBlitImage gfx::rec::CmdBlitImage
#define vkCmdCopyBufferToImage gfx::rec::CmdCopyBufferToImage
#define vkCmdCopyImageToBuffer gfx::rec::CmdCopyImageToBuffer
#define vkCmdClearColorImage gfx::rec::CmdClearColorImage
#define vkCmdClearDepthStencilImage gfx::rec::CmdClearDepthStencilImage
#define vkCmdResetQueryPool gfx::rec::CmdResetQueryPool
#define vkCmdWriteTimestamp gfx::rec::CmdWriteTimestamp
#endif
