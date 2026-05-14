#include "vram_bridge_capture_effect.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "core/variant/variant.h"

static const char *COPY_SHADER_GLSL = R"(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform readonly image2D src_image;
layout(std430,  set = 0, binding = 1) buffer DstBuffer {
    vec4 pixels[];
};

layout(push_constant) uniform CopyTextureData {
    uint width;
    uint height;
	uint pad0;
    uint pad1;
} data;

void main() {
    uvec2 uv = gl_GlobalInvocationID.xy;
    if (uv.x >= data.width || uv.y >= data.height) return;

    vec4 color = imageLoad(src_image, ivec2(uv));
    pixels[uv.y * data.width + uv.x] = color;
}
)";

static uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if (typeFilter & (1 << i) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}
	return ~0;
}

#if defined(WINDOWS_ENABLED)
#include <dxgi1_2.h>
#include <windows.h>
#include <VersionHelpers.h>
#include <aclapi.h>
#include <vulkan/vulkan_win32.h>

class WindowsSecurityAttributes
{
protected:
	SECURITY_ATTRIBUTES m_winSecurityAttributes;
	PSECURITY_DESCRIPTOR m_winPSecurityDescriptor;

public:
	WindowsSecurityAttributes();
	SECURITY_ATTRIBUTES *operator&();
	~WindowsSecurityAttributes();
};

WindowsSecurityAttributes::WindowsSecurityAttributes()
{
	m_winPSecurityDescriptor = (PSECURITY_DESCRIPTOR)calloc(1, SECURITY_DESCRIPTOR_MIN_LENGTH + 2 * sizeof(void **));
	if (!m_winPSecurityDescriptor) {
		throw std::runtime_error("Failed to allocate memory for security descriptor");
	}

	PSID *ppSID = (PSID *)((PBYTE)m_winPSecurityDescriptor + SECURITY_DESCRIPTOR_MIN_LENGTH);
	PACL *ppACL = (PACL *)((PBYTE)ppSID + sizeof(PSID *));

	InitializeSecurityDescriptor(m_winPSecurityDescriptor, SECURITY_DESCRIPTOR_REVISION);

	SID_IDENTIFIER_AUTHORITY sidIdentifierAuthority = SECURITY_WORLD_SID_AUTHORITY;
	AllocateAndInitializeSid(&sidIdentifierAuthority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, ppSID);

	EXPLICIT_ACCESS explicitAccess;
	ZeroMemory(&explicitAccess, sizeof(EXPLICIT_ACCESS));
	explicitAccess.grfAccessPermissions = STANDARD_RIGHTS_ALL | SPECIFIC_RIGHTS_ALL;
	explicitAccess.grfAccessMode = SET_ACCESS;
	explicitAccess.grfInheritance = INHERIT_ONLY;
	explicitAccess.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	explicitAccess.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	explicitAccess.Trustee.ptstrName = (LPTSTR)*ppSID;

	SetEntriesInAcl(1, &explicitAccess, NULL, ppACL);

	SetSecurityDescriptorDacl(m_winPSecurityDescriptor, TRUE, *ppACL, FALSE);

	m_winSecurityAttributes.nLength = sizeof(m_winSecurityAttributes);
	m_winSecurityAttributes.lpSecurityDescriptor = m_winPSecurityDescriptor;
	m_winSecurityAttributes.bInheritHandle = TRUE;
}

SECURITY_ATTRIBUTES *WindowsSecurityAttributes::operator&()
{
	return &m_winSecurityAttributes;
}

WindowsSecurityAttributes::~WindowsSecurityAttributes()
{
	PSID *ppSID = (PSID *)((PBYTE)m_winPSecurityDescriptor + SECURITY_DESCRIPTOR_MIN_LENGTH);
	PACL *ppACL = (PACL *)((PBYTE)ppSID + sizeof(PSID *));

	if (*ppSID) {
		FreeSid(*ppSID);
	}
	if (*ppACL) {
		LocalFree(*ppACL);
	}
	free(m_winPSecurityDescriptor);
}
#endif




void VRAMBridgeCaptureEffect::_bind_methods()
{
}

VRAMBridgeCaptureEffect::VRAMBridgeCaptureEffect()
{
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_TRANSPARENT);
	set_enabled(true);
	set_access_resolved_color(true); // Ensures MSAA is resolved before we read
}

VRAMBridgeCaptureEffect::~VRAMBridgeCaptureEffect()
{
	// Free Godot-side RIDs
	if (device) {
		if (m_pipeline_rid.is_valid()) {
			device->free_rid(m_pipeline_rid);
		}
		if (m_shader_rid.is_valid()) {
			device->free_rid(m_shader_rid);
		}
		if (m_godot_buf_rid.is_valid()) {
			device->free_rid(m_godot_buf_rid);
		}
	}

	if (vk_device != VK_NULL_HANDLE)
	{
		if (m_copy_cmd != VK_NULL_HANDLE)
		{
			vkFreeCommandBuffers(vk_device, m_copy_pool, 1, &m_copy_cmd);
		}
		if (m_copy_pool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(vk_device, m_copy_pool, nullptr);
		}
	}

	// ExternalBuffer destructor handles VkBuffer + CUDA cleanup
}

bool VRAMBridgeCaptureEffect::request_capture()
{
	State expected = State::IDLE;
	// Only transition IDLE → DISPATCHED; if already busy, return false
	return m_state.compare_exchange_strong(expected, State::DISPATCHED);
}

bool VRAMBridgeCaptureEffect::is_capture_complete() const
{
	return m_state.load() == State::COPY_PENDING;
}

/*
void VRAMBridgeCaptureEffect::release_cuda_buffer(cudaStream_t stream)
{
	// Signal vk_ready so Vulkan knows it can write again on the next capture
	cudaExternalSemaphoreSignalParams params{};
	params.flags = 0;
	cudaSignalExternalSemaphoresAsync(
			&m_ext_buf.cuda_signal_semaphore(), &params, 1, stream);

	// Transition COPY_PENDING → IDLE
	m_state.store(State::IDLE);
	m_needs_presignal = false; // vk_ready will be signalled by CUDA from now on
}*/

bool VRAMBridgeCaptureEffect::resources_valid() const
{
	return device && m_shader_rid.is_valid() && m_pipeline_rid.is_valid() && m_godot_buf_rid.is_valid() && m_copy_cmd_recorded;
}

void VRAMBridgeCaptureEffect::_render_callback(int p_effect_callback_type, RenderData *p_render_data)
{
	if (p_effect_callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT) return;

	auto *scene_buffers = Object::cast_to<RenderSceneBuffersRD>(*p_render_data->get_render_scene_buffers());
	if (!scene_buffers) return;

	Vector2i size = scene_buffers->get_internal_size();
	if (size.x == 0 || size.y == 0) return;

	uint32_t w = static_cast<uint32_t>(size.x);
	uint32_t h = static_cast<uint32_t>(size.y);

	// Lazy init (first callback that has a valid size) //Get the queue only
	if (!device)
	{
		device = RenderingServer::get_singleton()->get_rendering_device();
		if (!device) return;

		// Grab raw Vulkan handles from Godot
		vk_device = reinterpret_cast<VkDevice>(device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0));
		vk_physical_device = reinterpret_cast<VkPhysicalDevice>(device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE, RID(), 0));
		m_vk_queue = reinterpret_cast<VkQueue>(device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, RID(), 0));
		m_queue_family = static_cast<uint32_t>(device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_QUEUE_FAMILY, RID(), 0));
	}

	// (Re)allocate resources if size changed
	if (w != m_width || h != m_height)
	{
		// If a capture is in progress, don't resize — skip this frame
		if (m_state.load() != State::IDLE) return;
		initialize(w, h);
	}

	if (!resources_valid()) return;
	

	State current = m_state.load();

	// State: COPY_PENDING
	// Previous frame submitted the copy + signalled CUDA.
	// Nothing to do here — wait for release_cuda_buffer() call.
	if (current == State::COPY_PENDING) return;

	// State: DISPATCHED
	// The compute dispatch was recorded into Godot's DAG last frame.
	// Godot has now submitted it (we're in the NEXT frame's callback).
	// Safe to submit our copy now — it lands after last frame's GPU work.
	if (current == State::DISPATCHED)
	{
		// On the very first capture, vk_ready was never signalled.
		// We pre-signal it from Vulkan before the copy submit so the wait
		// doesn't block forever.
		if (m_needs_presignal)
		{
			presignal_vk_ready();
			m_needs_presignal = false;
		}

		submit_copy_with_semaphores(); //VRAMBridge::export_resource()

		// Transition DISPATCHED → COPY_PENDING
		m_state.store(State::COPY_PENDING);
		return;
	}

	// State: IDLE
	// Nothing requested — nothing to do.
	// (request_capture() transitions us to DISPATCHED on the next check.)
	//
	// But if someone called request_capture() between the last frame and now,
	// m_state is already DISPATCHED, which we handled above.
	// If it's still IDLE here, just return.
	//
	// Actually: request_capture() sets DISPATCHED immediately.
	// So the next time _render_callback fires after request_capture(),
	// m_state == DISPATCHED and we record the compute dispatch below.
	//
	// Wait — we need to re-read state after the DISPATCHED block above
	// may have returned.  Let's re-check: if we reach here, state is IDLE.
	// But request_capture() may have been called since we read it at the top.
	// Re-read:
	current = m_state.load();
	if (current != State::DISPATCHED) return; // still IDLE
	

	// Record compute dispatch into Godot's DAG
	// This is the frame where request_capture() was called (or the frame
	// immediately after, depending on timing).  Either way, we record now.

	// Get view 0's color image (we only capture view 0)
	RID color_tex = scene_buffers->get_internal_texture(0);

	Vector<RenderingDevice::Uniform> uniforms;
	// Binding 0: source color image (read-only storage image)
	RenderingDevice::Uniform img_uniform;
	img_uniform.uniform_type = RenderingDevice::UNIFORM_TYPE_IMAGE;
	img_uniform.binding = 0;
	img_uniform.append_id(color_tex);
	uniforms.push_back(img_uniform);

	// Binding 1: destination storage buffer (Godot-owned intermediate buffer)
	RenderingDevice::Uniform buf_uniform;
	buf_uniform.uniform_type = RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
	buf_uniform.binding = 1;
	buf_uniform.append_id(m_godot_buf_rid);
	uniforms.push_back(buf_uniform);

	RID uniform_set = device->uniform_set_create(uniforms, m_shader_rid, 0);
	if (!uniform_set.is_valid()) {
		ERR_FAIL_MSG("VRAMBridgeCaptureEffect: uniform_set_create failed");
		m_state.store(State::IDLE);
		return;
	}

	// Push constants: width + height (padded to 16 bytes = 4 x uint32)
	struct PushConstants {
		uint32_t width;
		uint32_t height;
		uint32_t pad0 = 0;
		uint32_t pad1 = 0;
	} pc{ w, h };

	PackedByteArray pc_bytes;
	pc_bytes.resize(sizeof(PushConstants));
	memcpy(pc_bytes.ptrw(), &pc, sizeof(PushConstants));

	// Dispatch the compute shader
	int64_t cl = device->compute_list_begin();
	device->compute_list_bind_compute_pipeline(cl, m_pipeline_rid);
	device->compute_list_bind_uniform_set(cl, uniform_set, 0);
	device->compute_list_set_push_constant(cl, pc_bytes.ptr(), sizeof(PushConstants));
	device->compute_list_dispatch(cl, (w + 7u) / 8u, (h + 7u) / 8u, 1);
	device->compute_list_end();

	// State stays DISPATCHED — next frame we will submit the copy
}

void VRAMBridgeCaptureEffect::initialize(uint32_t width, uint32_t height)
{
	// Free old resources
	if (m_godot_buf_rid.is_valid())
	{
		device->free_rid(m_godot_buf_rid);
		m_godot_buf_rid = RID();
	}
	if (m_copy_cmd != VK_NULL_HANDLE && vk_device != VK_NULL_HANDLE)
	{
		vkFreeCommandBuffers(vk_device, m_copy_pool, 1, &m_copy_cmd);
		m_copy_cmd = VK_NULL_HANDLE;
		m_copy_cmd_recorded = false;
	}
	destroy_external_buffer();

	m_width = width;
	m_height = height;

	// Size in bytes: width * height * 4 channels * 4 bytes (float32)
	// Note: Godot's color buffer is rgba16f internally, but we convert to
	// float32 in the shader for CUDA compatibility.
	VkDeviceSize buf_size = static_cast<VkDeviceSize>(width) * height * 4 * sizeof(float);

	// Create the buffer that will be manged by Godot:
	m_godot_buf_rid = device->storage_buffer_create(buf_size);
	if (!m_godot_buf_rid.is_valid())
	{
		ERR_FAIL_MSG("VRAMBridgeCaptureEffect: storage_buffer_create failed");
		return;
	}

	if (!m_shader_rid.is_valid())
	{
		build_shader();
		if (!m_shader_rid.is_valid())
		{
			return;
		}
	}

	// Create External buffer (CUDA-importable)
	try
	{
		init_external_buffer(
			VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			m_queue_family,
			buf_size,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
#if defined(WINDOWS_ENABLED)
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
#else
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
#endif
			0
		);
	}
	catch (const std::exception &e)
	{
		ERR_FAIL_MSG(String("VRAMBridgeCaptureEffect: ExternalBuffer::init failed: ") + e.what());
		return;
	}

	// Command pool and command buffer for the copy submit
	if (m_copy_pool == VK_NULL_HANDLE)
	{
		VkCommandPoolCreateInfo pool_info{};
		pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.queueFamilyIndex = m_queue_family;
		pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

		if (vkCreateCommandPool(vk_device, &pool_info, nullptr, &m_copy_pool) != VK_SUCCESS)
		{
			ERR_FAIL_MSG("VRAMBridgeCaptureEffect: vkCreateCommandPool failed");
			return;
		}
	}

	if (m_copy_cmd == VK_NULL_HANDLE)
	{
		VkCommandBufferAllocateInfo cb_info{};
		cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cb_info.commandPool = m_copy_pool;
		cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cb_info.commandBufferCount = 1;

		if (vkAllocateCommandBuffers(vk_device, &cb_info, &m_copy_cmd) != VK_SUCCESS)
		{
			ERR_FAIL_MSG("VRAMBridgeCaptureEffect: vkAllocateCommandBuffers failed");
			return;
		}
	}

	record_copy_command(width, height);
	m_needs_presignal = true; // reset so next capture pre-signals vk_ready
}

void VRAMBridgeCaptureEffect::init_external_buffer(VkBufferUsageFlags usage, uint32_t queue_family_index, VkDeviceSize size_bytes, VkMemoryPropertyFlags properties, VkExternalMemoryHandleTypeFlagsKHR extMemHandleType, int cuda_device_index) {
	external_buffer_size = size_bytes;
	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = external_buffer_size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkExternalMemoryBufferCreateInfo externalMemoryBufferInfo = {};
	externalMemoryBufferInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
	externalMemoryBufferInfo.handleTypes = extMemHandleType;
	bufferInfo.pNext = &externalMemoryBufferInfo;

	if (vkCreateBuffer(vk_device, &bufferInfo, nullptr, &vk_external_buffer) != VK_SUCCESS) {
		throw std::runtime_error("failed to create buffer!");
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(vk_device, vk_external_buffer, &memRequirements);

#if defined(WINDOWS_ENABLED)
	WindowsSecurityAttributes winSecurityAttributes;

	VkExportMemoryWin32HandleInfoKHR vulkanExportMemoryWin32HandleInfoKHR = {};
	vulkanExportMemoryWin32HandleInfoKHR.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
	vulkanExportMemoryWin32HandleInfoKHR.pNext = NULL;
	vulkanExportMemoryWin32HandleInfoKHR.pAttributes = &winSecurityAttributes;
	vulkanExportMemoryWin32HandleInfoKHR.dwAccess = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
	vulkanExportMemoryWin32HandleInfoKHR.name = (LPCWSTR)NULL;
#endif /* _WIN64 */
	VkExportMemoryAllocateInfoKHR vulkanExportMemoryAllocateInfoKHR = {};
	vulkanExportMemoryAllocateInfoKHR.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_KHR;
#if defined(WINDOWS_ENABLED)
	vulkanExportMemoryAllocateInfoKHR.pNext = extMemHandleType & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR
			? &vulkanExportMemoryWin32HandleInfoKHR
			: NULL;
	vulkanExportMemoryAllocateInfoKHR.handleTypes = extMemHandleType;
#else
	vulkanExportMemoryAllocateInfoKHR.pNext = NULL;
	vulkanExportMemoryAllocateInfoKHR.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif /* _WIN64 */
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.pNext = &vulkanExportMemoryAllocateInfoKHR;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = findMemoryType(vk_physical_device, memRequirements.memoryTypeBits, properties);

	if (vkAllocateMemory(vk_device, &allocInfo, nullptr, &vk_external_buffer_memory) != VK_SUCCESS) {
		throw std::runtime_error("failed to allocate external buffer memory!");
	}

	vkBindBufferMemory(vk_device, vk_external_buffer, vk_external_buffer_memory, 0);
}

void VRAMBridgeCaptureEffect::destroy_external_buffer()
{
	/*
	if (m_cuda_ready) {
		cudaDestroyExternalSemaphore(m_cuda_ready);
		m_cuda_ready = nullptr;
	}
	if (m_cuda_done) {
		cudaDestroyExternalSemaphore(m_cuda_done);
		m_cuda_done = nullptr;
	}
	if (m_cuda_ptr) {
		cudaFree(m_cuda_ptr);
		m_cuda_ptr = nullptr;
	}
	if (m_cuda_ext_mem) {
		cudaDestroyExternalMemory(m_cuda_ext_mem);
		m_cuda_ext_mem = nullptr;
	}*/

	if (vk_device != VK_NULL_HANDLE) {
		if (vk_ready_semaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(vk_device, vk_ready_semaphore, nullptr);
			vk_ready_semaphore = VK_NULL_HANDLE;
		}
		if (vk_done_semaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(vk_device, vk_done_semaphore, nullptr);
			vk_done_semaphore = VK_NULL_HANDLE;
		}
		if (vk_external_buffer != VK_NULL_HANDLE) {
			vkDestroyBuffer(vk_device, vk_external_buffer, nullptr);
			vk_external_buffer = VK_NULL_HANDLE;
		}
		if (vk_external_buffer_memory != VK_NULL_HANDLE) {
			vkFreeMemory(vk_device, vk_external_buffer_memory, nullptr);
			vk_external_buffer_memory = VK_NULL_HANDLE;
		}
	}
	vk_device = VK_NULL_HANDLE;
	external_buffer_size = 0;
}

void VRAMBridgeCaptureEffect::build_shader()
{
	String error;
	Vector<uint8_t> spirv_bytecode = device->shader_compile_spirv_from_source(
		RenderingDevice::SHADER_STAGE_COMPUTE,
		COPY_SHADER_GLSL,
		RenderingDevice::SHADER_LANGUAGE_GLSL,
		&error,
		false
	);
	RenderingDevice::ShaderStageSPIRVData shaderData;
	shaderData.shader_stage = RenderingDevice::SHADER_STAGE_COMPUTE;
	ERR_FAIL_COND_MSG(!error.is_empty(), "VRAMBridgeCaptureEffect: Can't create copy shader from errored bytecode.");
	shaderData.spirv = spirv_bytecode;

	m_shader_rid = device->shader_create_from_spirv({ shaderData }, "VRAMBridgeCaptureEffect:Copy");
	if (!m_shader_rid.is_valid()) {
		ERR_FAIL_MSG("VRAMBridgeCaptureEffect: shader_create_from_spirv failed");
		return;
	}

	m_pipeline_rid = device->compute_pipeline_create(m_shader_rid);
	if (!m_pipeline_rid.is_valid()) {
		ERR_FAIL_MSG("VRAMBridgeCaptureEffect: compute_pipeline_create failed");
		return;
	}
}

void VRAMBridgeCaptureEffect::record_copy_command(uint32_t width, uint32_t height)
{
	VkDeviceSize buf_size = static_cast<VkDeviceSize>(width) * height * 4 * sizeof(float);

	// Get the underlying VkBuffer for Godot's storage buffer
	VkBuffer godot_vk_buf = reinterpret_cast<VkBuffer>(device->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_BUFFER, m_godot_buf_rid, 0));
	if (godot_vk_buf == VK_NULL_HANDLE)
	{
		ERR_FAIL_MSG("VRAMBridgeCaptureEffect: get_driver_resource(BUFFER) returned null");
		return;
	}

	// Reset + begin
	vkResetCommandBuffer(m_copy_cmd, 0);

	VkCommandBufferBeginInfo begin_info{};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	// SIMULTANEOUS_USE so we can re-submit without waiting for previous to finish.
	// Safe here because we're gating re-submission on the CUDA → vk_ready semaphore.
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;             // ----------------- CHECK THIS

	vkBeginCommandBuffer(m_copy_cmd, &begin_info);

	// Barrier 1: wait for compute shader write on godot_buf
	VkBufferMemoryBarrier barrier1{};
	barrier1.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier1.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;  // srcStage:  COMPUTE_SHADER (where the dispatch wrote)
	barrier1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; // dstStage:  TRANSFER       (where vkCmdCopyBuffer reads)
	barrier1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier1.buffer = godot_vk_buf;
	barrier1.offset = 0;
	barrier1.size = VK_WHOLE_SIZE;

	vkCmdPipelineBarrier(m_copy_cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0,
		0, nullptr,
		1, &barrier1,
		0, nullptr
	);

	// Copy godot_buf -> ext_buf
	VkBufferCopy copy_region{};
	copy_region.srcOffset = 0;
	copy_region.dstOffset = 0;
	copy_region.size = buf_size;

	vkCmdCopyBuffer(m_copy_cmd, godot_vk_buf, vk_external_buffer, 1, &copy_region);

	// ── Barrier 2: release ext_buf to the external queue family (CUDA) ────
	// This is a queue family ownership transfer — release from our queue to
	// VK_QUEUE_FAMILY_EXTERNAL.  CUDA performs the matching acquire implicitly
	// when it accesses the buffer via the imported memory.
	VkBufferMemoryBarrier barrier2{};
	barrier2.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier2.dstAccessMask = 0; // CUDA doesn't need an explicit dstAccess
	barrier2.srcQueueFamilyIndex = m_queue_family;
	barrier2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; // hand to CUDA
	barrier2.buffer = vk_external_buffer;
	barrier2.offset = 0;
	barrier2.size = VK_WHOLE_SIZE;

	vkCmdPipelineBarrier(m_copy_cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0,
		0, nullptr,
		1, &barrier2,
		0, nullptr
	);

	vkEndCommandBuffer(m_copy_cmd);
	m_copy_cmd_recorded = true;
}

void VRAMBridgeCaptureEffect::presignal_vk_ready()
{
	// Submit an empty command buffer that signals vk_ready
	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = &vk_done_semaphore;

	// We want to signal vk_ready (the one Vulkan waits on before copy).
	// vk_ready is signalled by CUDA normally, but on first run we do it here.
	VkSemaphore ready_sem = vk_ready_semaphore; 
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = &ready_sem;

	vkQueueSubmit(m_vk_queue, 1, &submit_info, VK_NULL_HANDLE);
}

void VRAMBridgeCaptureEffect::submit_copy_with_semaphores()
{
	VkSemaphore wait_sem = vk_ready_semaphore; // vk_ready
	VkSemaphore signal_sem = vk_done_semaphore; // vk_done

	// The copy must not start until TRANSFER stage, but we also need the
	// barrier inside m_copy_cmd to fire at COMPUTE stage.  The wait mask
	// here only controls when the semaphore wait releases. Set ALL_COMMANDS
	// to be safe (semaphore waits happen at queue level anyway).
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.waitSemaphoreCount = 1;
	submit_info.pWaitSemaphores = &wait_sem;
	submit_info.pWaitDstStageMask = &wait_stage;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &m_copy_cmd;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = &signal_sem;

	VkResult result = vkQueueSubmit(m_vk_queue, 1, &submit_info, VK_NULL_HANDLE);
	if (result != VK_SUCCESS)
	{
		ERR_FAIL_MSG(String("VRAMBridgeCaptureEffect: vkQueueSubmit failed: ") + String::num_int64(static_cast<int64_t>(result)));
		m_state.store(State::IDLE);
	}
}
