#pragma once
#include "core/object/object.h"
#include "scene/resources/compositor.h"

//#include <cuda_runtime.h> //add cuda dempendency
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>

//#include "external_buffer.h"

//
// CaptureEffect
//
// A CompositorEffect that captures the Godot color buffer into an external
// VkBuffer on demand (not every frame).
//
// Strategy:
//   State machine with 3 states:
//     IDLE         — no capture in progress, Vulkan free to be asked again
//     DISPATCHED   — compute was recorded into Godot's DAG this frame;
//                    next frame we submit the copy
//     COPY_PENDING — copy submitted, semaphore signalled; CUDA is reading;
//                    waiting for CUDA to signal vk_ready before returning to IDLE
//
// Triggering a capture:
//   Call request_capture() from any thread.  The next _render_callback will
//   record the compute dispatch.  The frame after that, the vkCmdCopyBuffer
//   is submitted and the CUDA semaphore is signalled.
//
// Getting the data:
//   After requesting a capture, poll is_capture_complete().  When true, the
//   ExternalBuffer's cuda_ptr() contains the pixel data.  Call
//   release_cuda_buffer() when your CUDA kernel finishes, which re-arms
//   the Vulkan side for the next capture.
//
class VRAMBridgeCaptureEffect : public CompositorEffect
{
	GDCLASS(VRAMBridgeCaptureEffect, CompositorEffect)

public:
	VRAMBridgeCaptureEffect();
	~VRAMBridgeCaptureEffect();

	// Called by Godot. Do not call directly
	void _render_callback(int p_effect_callback_type, RenderData *p_render_data);

	// Request a capture. Safe to call from any thread. Ignored if a capture is already in progress.
	// Returns false if already busy.
	bool request_capture();

	// True once the copy submit has been issued and the CUDA semaphore has been signalled.
	bool is_capture_complete() const;

	// Call this after your CUDA work on the buffer is done.
	// Signals vk_ready so Vulkan knows CUDA has released the buffer.
	// This resets the state back to IDLE.
	//void release_cuda_buffer(cudaStream_t stream = 0);

	// Direct access to the external buffer for setting up torch::from_blob etc.
	//ExternalBuffer &get_external_buffer() { return m_ext_buf; }
	//const ExternalBuffer &get_external_buffer() const { return m_ext_buf; }

	// Image dimensions of the last dispatched capture
	uint32_t capture_width() const { return m_width; }
	uint32_t capture_height() const { return m_height; }

protected:
	static void _bind_methods();

private:
	//
	// First-frame vk_ready pre-signal: On the very first capture, vk_ready hasn't been signalled by CUDA yet
	// (CUDA hasn't run).  We pre-signal it from Vulkan once to get things going.
	//
	bool m_needs_presignal = true;

	uint32_t m_width = 0;
	uint32_t m_height = 0;

	enum class State : int {
		IDLE = 0, // nothing happening
		DISPATCHED = 1, // compute in DAG, waiting one frame before copy
		COPY_PENDING = 2, // copy submitted + semaphore signalled, CUDA reading
	};
	std::atomic<State> m_state{ State::IDLE };

	RenderingDevice *device = nullptr;
	RID m_shader_rid;
	RID m_pipeline_rid;
	RID m_godot_buf_rid; // intermediate storage buffer (Godot-owned)

	//TODO: Only the queue is necessary. There is no need for the rest (VRAMBridgeHooks has them)
	VkDevice vk_device = VK_NULL_HANDLE;
	VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
	VkQueue m_vk_queue = VK_NULL_HANDLE;
	uint32_t m_queue_family = 0;

	VkCommandPool m_copy_pool = VK_NULL_HANDLE;
	VkCommandBuffer m_copy_cmd = VK_NULL_HANDLE;
	bool m_copy_cmd_recorded = false;

	// EXTERNAL BUFFER ////////////////////////////////////////////////////////////////
	VkBuffer vk_external_buffer = VK_NULL_HANDLE;
	VkDeviceMemory vk_external_buffer_memory = VK_NULL_HANDLE;
	VkDeviceSize external_buffer_size = 0;

	VkSemaphore vk_done_semaphore = VK_NULL_HANDLE; //signaled when we finish copying to the external buffer (cuda waits this one before reading)
	VkSemaphore vk_ready_semaphore = VK_NULL_HANDLE; //signaled when the external application finishes reading the external buffer (Vulkan waits before next copy)

	//TODO: USE VMA INSTEAD TO ALLOCATE AND MANAGE THE BUFFER


	//////////////////////////////////////////////////////////////////////////////////



	// (re)create all resources for a given resolution
	void initialize(uint32_t width, uint32_t height);

	/////////////////////////////////////////////////////////////////// EXTERNAL BUFFER /////////////////////////////////////////////////////////////////////////////

	void init_external_buffer(VkBufferUsageFlags usage, uint32_t queue_family_index, VkDeviceSize size_bytes, VkMemoryPropertyFlags properties, VkExternalMemoryHandleTypeFlagsKHR extMemHandleType, int cuda_device_index = 0);
	void destroy_external_buffer();
	///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	void build_shader();

	//Pre-records the vkCmdCopyBuffer into m_copy_cmd. It will then be reusable for doing copies for exporting memory
	void record_copy_command(uint32_t width, uint32_t height);
	void submit_copy_with_semaphores();

	// On the first capture, signals vk_ready from Vulkan
	// so the wait in submit_copy_with_semaphores doesn't block forever. -------------------> THIS IS ENTIRELY AVOIDED WITH TIMELINE SEMAPHORES
	void presignal_vk_ready();

	bool resources_valid() const;
};

