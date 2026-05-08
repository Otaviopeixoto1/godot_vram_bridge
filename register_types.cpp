#include "register_types.h"
#include "drivers/vulkan/vulkan_hooks.h"
#include "core/config/project_settings.h"
#include "vram_bridge_vk_hooks.h"

static VRAMBridgeVKHooks *vram_bridge_hooks = nullptr;

void initialize_godot_vram_bridge_module(ModuleInitializationLevel p_level)
{
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS)
	{
		if (GLOBAL_GET("xr/openxr/enabled").booleanize())
		{
			// OpenXR (the only other VulkanHooks object) is initialized in SERVERS level... we need to init after it to be able to hook it together
			// BUT it cannot be done after the DisplayServer is created, which is right after SERVERS level...
			// This REALLY sucks but rather than modifying the engine source code i will just accept that openXR will just override this extension
			print_line("GodotVRAMBridge: Initialization Failed due to OpenXR being enabled. Currently this module is incompatible with OpenXR and will not be initialized");
			return;
		}
		if (VulkanHooks::get_singleton())
		{
			//TODO: IF POSSIBLE WE COULD WRAP THE PREVIOUSLY BOUND HOOKS AND FORWARD CALLS INTO THEM...
		}

		// Insert vulkan hook here to add the new extensions:
		vram_bridge_hooks = memnew(VRAMBridgeVKHooks);

		//WARN_PRINT("Loading additional Vulkan extensions");
		//VulkanContext::register_requested_instance_extension(VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, true);
		//VulkanContext::register_requested_instance_extension(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, true);

		//VulkanContext::register_requested_device_extension(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, true);
		//VulkanContext::register_requested_device_extension(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, true);
		//VulkanContext::register_requested_device_extension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, true);
		//VulkanContext::register_requested_device_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, true);
		//VulkanContext::register_requested_device_extension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, true);
	}
}

void uninitialize_godot_vram_bridge_module(ModuleInitializationLevel p_level)
{
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	if (vram_bridge_hooks)
	{
		vram_bridge_hooks->finish();
		memdelete(vram_bridge_hooks);
		vram_bridge_hooks = nullptr;
	}
	
}
