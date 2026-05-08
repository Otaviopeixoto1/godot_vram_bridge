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

	void finish();

private:
	struct DeviceQueueFamilies
	{
		TightLocalVector<VkQueueFamilyProperties> properties;
	};

	void _check_driver_workarounds(const VkPhysicalDeviceProperties &p_device_properties, RenderingContextDriver::Device &r_device);
	bool _device_supports_present(VkPhysicalDevice device, const DeviceQueueFamilies &queue_families);
	bool _queue_family_supports_present(VkPhysicalDevice device, uint32_t queue_family_index);


};
