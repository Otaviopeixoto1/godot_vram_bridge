#pragma once
#include "core/object/object.h"
#include "drivers/vulkan/vulkan_hooks.h"
#include "drivers/vulkan/rendering_context_driver_vulkan.h"


class VRAMBridgeVKHooks : public Object, VulkanHooks
{
	GDCLASS(VRAMBridgeVKHooks, Object);

protected:
	VkInstance vulkan_instance = nullptr;
	VkPhysicalDevice vulkan_physical_device = nullptr;
	VkDevice vulkan_device = nullptr;

	struct VkFunctions
	{
		PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR vkGetPhysicalDeviceExternalBufferProperties = nullptr;
	} vkFunctions;

	static void _bind_methods();

public:
	VRAMBridgeVKHooks() = default;
	virtual ~VRAMBridgeVKHooks() override = default;

	virtual bool create_vulkan_instance(const VkInstanceCreateInfo *p_vulkan_create_info, VkInstance *r_instance) override;
	virtual bool get_physical_device(VkPhysicalDevice *r_device) override;
	virtual bool create_vulkan_device(const VkDeviceCreateInfo *p_device_create_info, VkDevice *r_device) override;
	virtual void set_direct_queue_family_and_index(uint32_t p_queue_family_index, uint32_t p_queue_index) override;
	virtual bool use_fragment_density_offsets() override;
	virtual void get_fragment_density_offsets(LocalVector<VkOffset2D> &r_offsets, const Vector2i &p_granularity) override;


	//
	// Add methods for creating external buffers HERE
	// VMA IS ONLY SUPPORTED FOR WINDOW
	//

	// 
	// When initializing this module, the physical device is chosen such that it alway supports external buffers with usage flags: VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT.
	// This method can then be used to verify if the device supports any othor buffer usages for external buffers.
	// Returns true if external buffers with the given usage flags are supported in the current physical device.
	//
	bool device_supports_external_buffer(VkBufferUsageFlags usage);
	void finish();

private:
	struct DeviceQueueFamilies
	{
		TightLocalVector<VkQueueFamilyProperties> properties;
	};

	bool _insert_external_mem_instance_extensions(HashSet<CharString> &enabled_instance_extension_names);
	bool _insert_external_mem_device_extensions(HashSet<CharString> &enabled_device_extension_names);

	void _check_driver_workarounds(const VkPhysicalDeviceProperties &p_device_properties, RenderingContextDriver::Device &r_device);
	bool _device_supports_present(VkPhysicalDevice device, const DeviceQueueFamilies &queue_families);

	// Returns true if external buffers with the given usage flags are supported in the current physical device usage flags defaults to export buffers
	bool _device_supports_external_buffer(VkPhysicalDevice device, VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	bool _queue_family_supports_present(VkPhysicalDevice device, uint32_t queue_family_index);


};
