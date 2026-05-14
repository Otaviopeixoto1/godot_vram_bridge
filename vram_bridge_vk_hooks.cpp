#include "vram_bridge_vk_hooks.h"
#include "core/templates/vector.h"
#include "drivers/vulkan/rendering_device_driver_vulkan.h"

#include <vulkan/vulkan_win32.h>

//
// Taken from godot 4.6: rendering_device.cpp
//

/**************************/
/**** HELPER FUNCTIONS ****/
/**************************/

static String _get_device_vendor_name(const RenderingContextDriver::Device &p_device) {
	switch (p_device.vendor) {
		case RenderingContextDriver::Vendor::VENDOR_AMD:
			return "AMD";
		case RenderingContextDriver::Vendor::VENDOR_IMGTEC:
			return "ImgTec";
		case RenderingContextDriver::Vendor::VENDOR_APPLE:
			return "Apple";
		case RenderingContextDriver::Vendor::VENDOR_NVIDIA:
			return "NVIDIA";
		case RenderingContextDriver::Vendor::VENDOR_ARM:
			return "ARM";
		case RenderingContextDriver::Vendor::VENDOR_MICROSOFT:
			return "Microsoft";
		case RenderingContextDriver::Vendor::VENDOR_QUALCOMM:
			return "Qualcomm";
		case RenderingContextDriver::Vendor::VENDOR_INTEL:
			return "Intel";
		default:
			return "Unknown";
	}
}

static String _get_device_type_name(const RenderingContextDriver::Device &p_device) {
	switch (p_device.type) {
		case RenderingContextDriver::DEVICE_TYPE_INTEGRATED_GPU:
			return "Integrated";
		case RenderingContextDriver::DEVICE_TYPE_DISCRETE_GPU:
			return "Discrete";
		case RenderingContextDriver::DEVICE_TYPE_VIRTUAL_GPU:
			return "Virtual";
		case RenderingContextDriver::DEVICE_TYPE_CPU:
			return "CPU";
		case RenderingContextDriver::DEVICE_TYPE_OTHER:
		default:
			return "Other";
	}
}

static uint32_t _get_device_type_score(const RenderingContextDriver::Device &p_device) {
	static const bool prefer_integrated = OS::get_singleton()->get_user_prefers_integrated_gpu();
	switch (p_device.type) {
		case RenderingContextDriver::DEVICE_TYPE_INTEGRATED_GPU:
			return prefer_integrated ? 5 : 4;
		case RenderingContextDriver::DEVICE_TYPE_DISCRETE_GPU:
			return prefer_integrated ? 4 : 5;
		case RenderingContextDriver::DEVICE_TYPE_VIRTUAL_GPU:
			return 3;
		case RenderingContextDriver::DEVICE_TYPE_CPU:
			return 2;
		case RenderingContextDriver::DEVICE_TYPE_OTHER:
		default:
			return 1;
	}
}


/***************************/
/**** VRAMBridgeVKHooks ****/
/***************************/

void VRAMBridgeVKHooks::_bind_methods()
{
}

bool VRAMBridgeVKHooks::create_vulkan_instance(const VkInstanceCreateInfo *p_vulkan_create_info, VkInstance *r_instance)
{
	HashSet<CharString> enabled_instance_extension_names;
	for (uint32_t i = 0; i < p_vulkan_create_info->enabledExtensionCount; i++)
	{
		//push all instance extensions that are currently already enabled to enabled_instance_extension_names
		enabled_instance_extension_names.insert(p_vulkan_create_info->ppEnabledExtensionNames[i]);
	}

	bool insertSuccess = _insert_external_mem_instance_extensions(enabled_instance_extension_names);
	ERR_FAIL_COND_V_MSG(!insertSuccess, false,
			"Unable to load the required Vulkan Instance extensions.\n\n" //----------> TODO: Instead of returning error maybe just run normal vulkan ??
			"vkCreateInstance Failure");

	// Copy all settings and add the modified extension list:
	VkInstanceCreateInfo createInfo = *p_vulkan_create_info;

	// Set the new extensions:
	TightLocalVector<const char *> enabled_extension_names;
	enabled_extension_names.reserve(enabled_instance_extension_names.size());
	for (const CharString &extension_name : enabled_instance_extension_names)
	{
		enabled_extension_names.push_back(extension_name.ptr());
	}

	createInfo.enabledExtensionCount = enabled_extension_names.size();
	createInfo.ppEnabledExtensionNames = enabled_extension_names.ptr();

	// Create instance:
	VkResult vk_result = vkCreateInstance(&createInfo, VKC::get_allocation_callbacks(VK_OBJECT_TYPE_INSTANCE), &vulkan_instance);

	ERR_FAIL_COND_V_MSG(vk_result == VK_ERROR_INCOMPATIBLE_DRIVER, false,
				"Cannot find a compatible Vulkan installable client driver (ICD).\n\n"
				"vkCreateInstance Failure");
	ERR_FAIL_COND_V_MSG(vk_result == VK_ERROR_EXTENSION_NOT_PRESENT, false,
				"Cannot find a specified extension library.\n"
				"Make sure your layers path is set appropriately.\n"
				"vkCreateInstance Failure");
	ERR_FAIL_COND_V_MSG(vk_result, false,
				"vkCreateInstance failed.\n\n"
				"Do you have a compatible Vulkan installable client driver (ICD) installed?\n"
				"Please look at the Getting Started guide for additional information.\n"
				"vkCreateInstance Failure");

	// Create the function for querying physical device support to external buffers: 
	vkFunctions.vkGetPhysicalDeviceExternalBufferProperties = PFN_vkGetPhysicalDeviceExternalBufferProperties(vkGetInstanceProcAddr(vulkan_instance, "vkGetPhysicalDeviceExternalBufferProperties"));

	// In Vulkan 1.0, the functions might be accessible under their original extension names.
	if (vkFunctions.vkGetPhysicalDeviceExternalBufferProperties == nullptr)
	{
		vkFunctions.vkGetPhysicalDeviceExternalBufferProperties = PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR(vkGetInstanceProcAddr(vulkan_instance, "vkGetPhysicalDeviceExternalBufferPropertiesKHR"));
	}

	ERR_FAIL_COND_V_MSG(vkFunctions.vkGetPhysicalDeviceExternalBufferProperties == nullptr, false,
			"vkGetInstanceProcAddr Failure. Failed at getting the function pointer to vkGetPhysicalDeviceExternalBufferProperties.\n"
			"vkCreateInstance Failure"); //----------> TODO: Instead of returning error maybe just run normal vulkan ??

	*r_instance = vulkan_instance;
	return true;
}

bool VRAMBridgeVKHooks::get_physical_device(VkPhysicalDevice *r_device)
{
	// Here, out of all devices we must choose one !!! This will be the device that is used through the whole application !
	// CHECK ON RenderingContextDriverVulkan for a way to pick the right device !

	// Step 1: Create list of physical devices
	uint32_t physical_device_count = 0;
	VkResult err = vkEnumeratePhysicalDevices(vulkan_instance, &physical_device_count, nullptr);
	ERR_FAIL_COND_V(err != VK_SUCCESS, false);
	ERR_FAIL_COND_V_MSG(physical_device_count == 0, false, "vkEnumeratePhysicalDevices reported zero accessible devices.\n\nDo you have a compatible Vulkan installable client driver (ICD) installed?\nvkEnumeratePhysicalDevices Failure.");

	TightLocalVector<VkPhysicalDevice> physical_devices;
	physical_devices.resize(physical_device_count);
	err = vkEnumeratePhysicalDevices(vulkan_instance, &physical_device_count, physical_devices.ptr());
	ERR_FAIL_COND_V_MSG(err != VK_SUCCESS, false, "vkEnumeratePhysicalDevices failed at creating physical device list.\n\nDo you have a compatible Vulkan installable client driver (ICD) installed?\nvkEnumeratePhysicalDevices Failure.");

	// Step 2: Query Device Queue Properties
	TightLocalVector<RenderingContextDriver::Device> driver_devices;
	driver_devices.resize(physical_device_count);
	TightLocalVector<DeviceQueueFamilies> device_queue_families;
	device_queue_families.resize(physical_device_count);

	// Fill the list of driver devices with the properties from the physical devices.
	for (uint32_t i = 0; i < physical_devices.size(); i++)
	{
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(physical_devices[i], &props);

		RenderingContextDriver::Device &driver_device = driver_devices[i];
		driver_device.name = String::utf8(props.deviceName);
		driver_device.vendor = props.vendorID;
		driver_device.type = RenderingContextDriver::DeviceType(props.deviceType);
		driver_device.workarounds = RenderingContextDriver::Workarounds();

		_check_driver_workarounds(props, driver_device);

		uint32_t queue_family_properties_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[i], &queue_family_properties_count, nullptr);

		if (queue_family_properties_count > 0)
		{
			device_queue_families[i].properties.resize(queue_family_properties_count);
			vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[i], &queue_family_properties_count, device_queue_families[i].properties.ptr());
		}
	}

	// Step 3: pick a device based on scoring... (Must support graphics and present queues)
	//
	// Usually, we would use the main surface that is already built to quert for presentation support (see: RenderingDevice::initialize() in RenderingDevice.cpp)
	// However, at this stage, the RenderingDevice is not yet initialized, therfore we cannot use RenderingDevice::get_singleton().get_context_driver(). Godot also doesnt expose it to
	// the VulkanHooks. The best thing we can do is to use the platform specific versions of: vkGetPhysicalDevice*PresentationSupportKHR to query for genering presentation support,
	// which DOES NOT GUARANTEE that the selected device will truly support presentation for any specific VkSurface but it is the best that can be done right now...
	// (other than modifying the engine source)
	//

	print_verbose("DEVICE COUNT == " + itos(physical_device_count));
	int32_t device_index = Engine::get_singleton()->get_gpu_index(); //This is given as an argument when initializing godot with the CLI (by default its -1)
	const bool detect_device = (device_index < 0) || (device_index >= int32_t(physical_device_count));
	uint32_t device_type_score = 0;
	for (uint32_t i = 0; i < physical_device_count; i++)
	{
		RenderingContextDriver::Device device_option = driver_devices[i];
		String name = device_option.name;
		String vendor = _get_device_vendor_name(device_option);
		String type = _get_device_type_name(device_option);
		bool present_supported = _device_supports_present(physical_devices[i], device_queue_families[i]);

		//
		// Here we test for storage + transfer dst since that is the main application of this module: exporting buffers from godot to other applications.
		// When trying to create other types of buffers, always verify support using the public method: bool device_supports_external_buffer(VkBufferUsageFlags usage)
		//
		bool external_mem_supported = _device_supports_external_buffer(physical_devices[i], VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

		print_verbose("  #" + itos(i) + ": " + vendor + " " + name + " feature support state (present, external memory) = " + (present_supported ? "Supported" : "Unsupported") + ", " + (external_mem_supported ? "Supported" : "Unsupported") + ", " + type);
		if (detect_device && present_supported && external_mem_supported)
		{
			// If a window was specified, present must be supported by the device to be available as an option.
			// Assign a score for each type of device and prefer the device with the higher score.
			uint32_t option_score = _get_device_type_score(device_option);
			if (option_score > device_type_score)
			{
				device_index = i;
				device_type_score = option_score;
			}
		}
	}

	ERR_FAIL_COND_V_MSG((device_index < 0) || (device_index >= int32_t(physical_device_count)), false, "None of the devices supports both graphics and present queues.");

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(vulkan_physical_device, &props);

	vulkan_physical_device = physical_devices[device_index];
	*r_device = vulkan_physical_device;
	return true;
}

bool VRAMBridgeVKHooks::create_vulkan_device(const VkDeviceCreateInfo *p_device_create_info, VkDevice *r_device)
{
	HashSet<CharString> enabled_device_extension_names;
	for (uint32_t i = 0; i < p_device_create_info->enabledExtensionCount; i++)
	{
		//push all device extensions that are currently already enabled to enabled_instance_extension_names
		enabled_device_extension_names.insert(p_device_create_info->ppEnabledExtensionNames[i]);
	}

	bool insertSuccess = _insert_external_mem_device_extensions(enabled_device_extension_names);
	ERR_FAIL_COND_V_MSG(!insertSuccess, false,
			"Unable to load the required Vulkan Device extensions.\n\n" //----------> TODO: Instead of returning error maybe just run normal vulkan ??
			"vkCreateInstance Failure");

	// Copy all settings and add the modified extension list:
	VkDeviceCreateInfo createInfo = *p_device_create_info;

	// Set the new extensions:
	TightLocalVector<const char *> enabled_extension_names;
	enabled_extension_names.reserve(enabled_device_extension_names.size());
	for (const CharString &extension_name : enabled_device_extension_names)
	{
		enabled_extension_names.push_back(extension_name.ptr());
	}

	createInfo.enabledExtensionCount = enabled_extension_names.size();
	createInfo.ppEnabledExtensionNames = enabled_extension_names.ptr();

	VkResult vk_result = vkCreateDevice(vulkan_physical_device, &createInfo, VKC::get_allocation_callbacks(VK_OBJECT_TYPE_DEVICE), &vulkan_device);
	if (vk_result != VK_SUCCESS)
	{
		print_line("VRAMBridgeVKHooks: Failed to create Vulkan device [Vulkan error", vk_result, "]");
		return false;
	}

	*r_device = vulkan_device;

	//
	// TODO: VMA Support for External memory on windows only comes in v3.2.0 and godot currently uses 3.1.0 ...
	//

	return true;
}

void VRAMBridgeVKHooks::set_direct_queue_family_and_index(uint32_t p_queue_family_index, uint32_t p_queue_index)
{
	//This is triggered when the main queue is created, We can later store and use it for our own commands
}

bool VRAMBridgeVKHooks::use_fragment_density_offsets()
{
	return false;
}

void VRAMBridgeVKHooks::get_fragment_density_offsets(LocalVector<VkOffset2D> &r_offsets, const Vector2i &p_granularity)
{
	ERR_FAIL_MSG("VRAMBridgeVKHooks: fragment density offsets are not supported. use_fragment_density_offsets was set to false yet fragment density offsets were still requested !");
}

bool VRAMBridgeVKHooks::device_supports_external_buffer(VkBufferUsageFlags usage)
{
	return _device_supports_external_buffer(vulkan_physical_device, usage);
}

void VRAMBridgeVKHooks::finish() {
	//TODO: cleanup all vk allocations owned by this class
}

bool VRAMBridgeVKHooks::_insert_external_mem_instance_extensions(HashSet<CharString> &enabled_instance_extension_names)
{
	HashSet<CharString> requested_instance_extensions {
		VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
	};

	//
	// Based on godot 4.6: rendering_context_driver_vulkan.cpp
	//
	uint32_t instance_extension_count = 0;
	VkResult err = vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, nullptr);
	ERR_FAIL_COND_V(err != VK_SUCCESS && err != VK_INCOMPLETE, false);
	ERR_FAIL_COND_V_MSG(instance_extension_count == 0, false, "No instance extensions were found.");

	TightLocalVector<VkExtensionProperties> instance_extensions;
	instance_extensions.resize(instance_extension_count);
	err = vkEnumerateInstanceExtensionProperties(nullptr, &instance_extension_count, instance_extensions.ptr());
	if (err != VK_SUCCESS && err != VK_INCOMPLETE)
	{
		ERR_FAIL_V(false); //This should not be reached... It should only fail if the previous call would also fail
	}

	// Enable all extensions that are supported and requested.
	for (uint32_t i = 0; i < instance_extension_count; i++)
	{
		CharString extension_name(instance_extensions[i].extensionName);
		if (requested_instance_extensions.has(extension_name))
		{
			enabled_instance_extension_names.insert(extension_name);
		}
	}

	// Now check if our requested extensions were enabled (THEY ARE ALL REQUIRED...).
	for (const CharString &requested_extension : requested_instance_extensions)
	{
		if (!enabled_instance_extension_names.has(requested_extension))
		{
			ERR_FAIL_V_MSG(false, String("VRAMBridgeVKHooks: Required Instance Extension ") + String::utf8(requested_extension) + String(" not found."));
		}
	}
	return true;
}

bool VRAMBridgeVKHooks::_insert_external_mem_device_extensions(HashSet<CharString> &enabled_device_extension_names)
{
	HashSet<CharString> requested_device_extensions{
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
		//VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
#if defined(WINDOWS_ENABLED)
		VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#else
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
#endif
	};

	uint32_t device_extension_count = 0;
	VkResult err = vkEnumerateDeviceExtensionProperties(vulkan_physical_device, nullptr, &device_extension_count, nullptr);
	ERR_FAIL_COND_V(err != VK_SUCCESS, false);
	ERR_FAIL_COND_V_MSG(device_extension_count == 0, false, "vkEnumerateDeviceExtensionProperties failed to find any extensions\n\nDo you have a compatible Vulkan installable client driver (ICD) installed?");

	TightLocalVector<VkExtensionProperties> device_extensions;
	device_extensions.resize(device_extension_count);
	err = vkEnumerateDeviceExtensionProperties(vulkan_physical_device, nullptr, &device_extension_count, device_extensions.ptr());
	ERR_FAIL_COND_V(err != VK_SUCCESS, false);

	// Enable all extensions that are supported and requested.
	for (uint32_t i = 0; i < device_extension_count; i++)
	{
		CharString extension_name(device_extensions[i].extensionName);
		if (requested_device_extensions.has(extension_name))
		{
			enabled_device_extension_names.insert(extension_name);
		}
	}

	// Now check if our requested extensions were enabled (THEY ARE ALL REQUIRED...).
	for (const CharString &requested_extension : requested_device_extensions)
	{
		if (!enabled_device_extension_names.has(requested_extension))
		{
			ERR_FAIL_V_MSG(false, String("VRAMBridgeVKHooks: Required Device Extension ") + String::utf8(requested_extension) + String(" not found."));
		}
	}
	return true;
}

void VRAMBridgeVKHooks::_check_driver_workarounds(const VkPhysicalDeviceProperties &p_device_properties, RenderingContextDriver::Device &r_device)
{
	//
	// Taken From godot 4.6: rendering_context_driver_vulkan.cpp
	//

	// Workaround for the Adreno 6XX family of devices.
	//
	// There's a known issue with the Vulkan driver in this family of devices where it'll crash if a dynamic state for drawing is
	// used in a command buffer before a dispatch call is issued. As both dynamic scissor and viewport are basic requirements for
	// the engine to not bake this state into the PSO, the only known way to fix this issue is to reset the command buffer entirely.
	//
	// As the render graph has no built in limitations of whether it'll issue compute work before anything needs to draw on the
	// frame, and there's no guarantee that compute work will never be dependent on rasterization in the future, this workaround
	// will end recording on the current command buffer any time a compute list is encountered after a draw list was executed.
	// A new command buffer will be created afterwards and the appropriate synchronization primitives will be inserted.
	//
	// Executing this workaround has the added cost of synchronization between all the command buffers that are created as well as
	// all the individual submissions. This performance hit is accepted for the sake of being able to support these devices without
	// limiting the design of the renderer.
	//
	// This bug was fixed in driver version 512.503.0, so we only enabled it on devices older than this.
	//
	r_device.workarounds.avoid_compute_after_draw =
			r_device.vendor == RenderingContextDriver::Vendor::VENDOR_QUALCOMM &&
			p_device_properties.deviceID >= 0x6000000 && // Adreno 6xx
			p_device_properties.driverVersion < VK_MAKE_VERSION(512, 503, 0) &&
			r_device.name.find("Turnip") < 0;
}

bool VRAMBridgeVKHooks::_device_supports_present(VkPhysicalDevice device, const DeviceQueueFamilies &queue_families)
{
	for (uint32_t i = 0; i < queue_families.properties.size(); i++)
	{
		if ((queue_families.properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && _queue_family_supports_present(device, i))
		{
			return true;
		}
	}

	return false;
}

bool VRAMBridgeVKHooks::_device_supports_external_buffer(VkPhysicalDevice device, VkBufferUsageFlags usage)
{
	VkPhysicalDeviceExternalBufferInfoKHR extBufInfo{};
	extBufInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO_KHR;
	extBufInfo.usage = usage;
#if defined(WINDOWS_ENABLED)
	extBufInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR; //TODO: CHECK OTHER HANDLE TYPES
	//Todo: CHECK FOR OTHER PLATFORMS AS WELL (JUST LINUX FOR NOW)
#else
	extBufInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR; //TODO: CHECK IF THIS IS THE RIGHT HANDLE FOR ALL OTHER PLATFORMS
#endif

	VkExternalBufferPropertiesKHR extBufProps{};
	extBufProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES_KHR;

	vkFunctions.vkGetPhysicalDeviceExternalBufferProperties(device, &extBufInfo, &extBufProps);
	return (extBufProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT_KHR);
}

bool VRAMBridgeVKHooks::_queue_family_supports_present(VkPhysicalDevice device, uint32_t queue_family_index)
{
	//
	// Here we use the platform-specific functions: vkGetPhysicalDevice*PresentationSupportKHR to query presentation support for each queue family index in the given device.
	// This is not ideal but its the best implementation of this test given that we dont have access to the VkSurface object.
	// 
	VkBool32 supports_present = VK_FALSE;
#if defined(WINDOWS_ENABLED)
	supports_present = vkGetPhysicalDeviceWin32PresentationSupportKHR(device, queue_family_index);
	//Todo: CHECK FOR OTHER PLATFORMS AS WELL (JUST LINUX FOR NOW)
#endif

	return supports_present == VK_TRUE;
}


