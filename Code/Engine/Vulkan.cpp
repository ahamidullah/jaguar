#if defined(USING_VULKAN_API)

#include "Vulkan.h"
#include "Render.h"
#include "Math.h"

#include "Basic/DLL.h"
#include "Basic/Array.h"
#include "Basic/Log.h"

#include "Common.h"

const auto VulkanMaxFramesInFlight = 2;

// @TODO: Move this to render.c.
#define SHADOW_MAP_WIDTH  1024
#define SHADOW_MAP_HEIGHT 1024
#define SHADOW_MAP_INITIAL_LAYOUT GFX_IMAGE_LAYOUT_UNDEFINED
#define SHADOW_MAP_FORMAT GFX_FORMAT_D16_UNORM
#define SHADOW_MAP_IMAGE_USAGE_FLAGS ((GfxImageUsage)(GFX_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT | GFX_IMAGE_USAGE_SAMPLED))
#define SHADOW_MAP_SAMPLE_COUNT_FLAGS GFX_SAMPLE_COUNT_1
// Depth bias and slope are used to avoid shadowing artifacts.
// Constant depth bias factor is always applied.
#define SHADOW_MAP_CONSTANT_DEPTH_BIAS 0.25
// Slope depth bias factor is applied depending on the polygon's slope.
#define SHADOW_MAP_SLOPE_DEPTH_BIAS 1.25
#define SHADOW_MAP_FILTER GFX_LINEAR_FILTER

// @TODO: Make sure that all failable Vulkan calls are VK_CHECK'd.
// @TODO: Do eg.
//        typedef VImageLayout GFX_Image_Layout;
//        #define GFX_IMAGE_LAYOUT_1 VK_IMAGE_LAYOUT_1
//        etc... That will save us from having to convert between the two values.
// @TODO: Some kind of memory protection for the Gfx buffer!
// @TODO: Better texture descriptor set update scheme.
// @TODO: Move any uniform data that's updated per-frame to shared memory.
// @TODO: Scene stuff should be pbr?
// @TODO: Shouldn't per-swapchain resources actually be per-frame?
// @TODO: Query available VRAM and allocate based on that.
// @TODO: Switch to dynamic memory segments for vulkan Gfx memory.
//        If more VRAM becomes available while the game is running, the game should use it!
// @TODO: Change 'model' to local? E.g. 'model_to_world_space' -> 'local_to_world_space'.
// @TODO: Avoid VK_SHARING_MODE_CONCURRENT? "Go for VK_SHARING_MODE_EXCLUSIVE and do explicit queue family ownership barriers."
// @TODO: Dedicated transfer queue.
// @TODO: Experiment with HOST_CACHED memory?
// @TODO: Keep memory mapped persistently.
// @TODO: Use a single allocation and sub-allocations for image memory.
// @TODO: Different memory management system for shared allocations.
//        Main vertex, index, image = dynamic allocator, device memory
//        Uniforms = fixed size with offsets, device memory
//        Staging = stack allocator, shared memory, per-stage lifetime
//        Per-frame memory (debug/gui vertices/indices/uvs) = stack allocator, shared memory, per-frame lifetime
//        (Pool of blocks for shared memory)
// @TODO: Gfx memory defragmenting.
// @TODO: Debug clear freed memory?
// @TODO: Debug memory protection?
// @TODO: Read image pixels and mesh verices/indices directly into Gfx accessable staging memory.
// @TODO: What happens if MAX_FRAMES_IN_FLIGHT is less than or greater than the number of swapchain images?

enum VulkanQueueType
{
	VulkanGraphicsQueue,
	VulkanTransferQueue,
	VulkanComputeQueue,

	VulkanQueueTypeCount
};

enum VulkanBufferUsage
{
	VulkanVertexBuffer,
	VulkanIndexBuffer,
	VulkanUniformBuffer,

	VulkanBufferUsageCount
};

enum VulkanSync
{
	VulkanFrameSync,
	VulkanAsync,
};

struct VulkanMemoryAllocation
{
	VkDeviceMemory vk;
	s64 offset;
};

struct VulkanBufferData
{
	VulkanMemoryAllocation memory;
};

struct VulkanImageData
{
	VulkanMemoryAllocation memory;
};

struct VulkanMapData
{
	VkBuffer buffer;
	VulkanMemoryAllocation memory;
	#if DEBUG_BUILD
		bool async;
	#endif
};

enum VulkanMemoryType
{
	VulkanGPUOnlyMemory,
	VulkanCPUToGPUMemory,
	VulkanGPUToCPUMemory,

	VulkanMemoryTypeCount
};

struct VulkanThreadLocal
{
	// @TODO: Explain how frame command pools work.
	StaticArray<StaticArray<VKCommandPool, VkMaxFramesInFlight>, VulkanQueueTypeCount> framePools;
	Array<VkBuffer> frameTransferBuffers;
	Array<VulkanMemoryAllocation> frameTransferMemory;
};

auto vkThreadLocal = Array<VulkanThreadLocal>{};
auto vkDebugMessenger = VkDebugUtilsMessengerEXT{};
auto vkInstance = VkInstance{};
auto vkPhysicalDevice = VkPhysicalDevice{};
auto vkDevice = VkDevice{};
auto vkQueueFamilies = StaticArray<u32, VulkanQueueTypeCount>{};
auto vkQueues = StaticArray<VkQueue, VulkanQueueCount>;
auto vkChangeImageOwnershipFromGraphicsToPresentQueueCommands = Array<VkCommandBuffer>{};
auto vkPresentMode = VkPresentModeKHR{};
auto vkPresentCommandPool = VkCommandPool{};
auto vkPresentQueueFamily = u32{};
auto vkPresentQueue = VkQueue{};
auto vkSurface = VkSurfaceKHR{};
auto vkSurfaceFormat = VkSurfaceFormatKHR{};
auto vkBufferImageGranularity = s64{};
auto vkMemoryHeapCount = s64{};
auto vkMemoryTypeToMemoryFlags = StaticArray<VkMemoryPropertyFlags, VulkanMemoryTypeCount>{};
auto vkMemoryTypeToMemoryIndex = StaticArray<s64, VulkanMemoryTypeCount>{};
auto vkMemoryTypeToHeapIndex = StaticArray<s64, VulkanMemoryTypeCount>{};
auto vkBufferData = HashTable<VkBuffer, VulkanBufferData>{};
auto vkImageData = HashTable<VkImage, VulkanImageData>{};
auto vkMapData = HashTable<void *, VulkanMapData>{};
auto vkGPUBlockAllocator = VulkanMemoryBlockAllocator{};
auto vkCPUToGPUBlockAllocator = VulkanMemoryBlockAllocator{};
auto vkGPUToCPUBlockAllocator = VulkanMemoryBlockAllocator{};
auto vkCPUToGPURingAllocator = VulkanMemoryRingAllocator{};
// @TODO: Explain these and how they interact with the command queues...
// @TODO: We could store async queues in thread local storage and avoid locking.
auto vkAsyncQueueLocks = StaticArray<SpinLock, VulkanQueueTypeCount>{};
auto vkAsyncQueues = StaticArray<Array<VkCommandBuffer>, VulkanQueueTypeCount>{};
auto vkAsyncSignals StaticArray<Array<bool *>, VulkanQueueTypeCount>{};
auto vkAsyncQueueIndices = StaticArray<(volatile s64), VulkanQueueTypeCount>{}
auto vkAsyncCommandPools = StaticArray<VkCommandPool, VulkanQueueTypeCount>{}
// @TODO: Explain how command buffer queues work.
// @TODO: We could store frame queues in thread local storage and avoid locking.
auto vkFrameQueueLocks = StaticArray<SpinLock, VulkanQueueTypeCount>{};
auto vkFrameQueues = StaticArray<Array<VkCommandBuffer>, VulkanQueueTypeCount>{};
auto vkSwapchain = VkSwapchain{};
auto vkSwapchainImageIndex = s64{}; // @TODO
auto vkSwapchainImages = Array<VkImage>{};
auto vkSwapchainImageViews = Array<VkImageView>{};
auto vkPipelineLayout = VkPipelineLayout{};
auto vkDescriptorPool = VkDescriptorPool{};
auto vkDescriptorSetLayouts = Array<VkDescriptorSetLayout>{}; 
auto vkDescriptorSets = Array<Array<VkDescriptorSet{}>>{};
auto vkDescriptorSetBuffers = Array<Array<VKBuffer>>{};
auto vkDescriptorSetUpdateFence = VkFence{};
// @TODO: Explain how semaphores work and interact.
auto vkImageOwnershipSemaphores = StaticArray<VkSemaphore, VulkanMaxFramesInFlight>{};
auto vkImageAcquiredSemaphores = StaticArray<VkSemaphore, VulkanMaxFramesInFlight>{};
auto vkDrawCompleteSemaphores = StaticArray<VkSemaphore, VulkanMaxFramesInFlight>{};
// @TODO: Could be thread-local lock-free double buffer.
auto vkAsyncTransferResourceLock = SpinLock{};
auto vkAsyncTransferBuffers = Array<VkBuffer>{};
auto vkAsyncTransferMemory = Array<VulkanMemoryAllocation>{};
auto vkAsyncTransferSignals = Array<bool *>{};
auto vkFrameIndex = u32{};
auto vkFrameFences = StaticArray<VkFence, GFX_MAX_FRAMES_IN_FLIGHT>{};

#define CheckVkResult(x)\
	do {\
		VkResult _r = (x);\
		if (_r != VK_SUCCESS) Abort("VK_CHECK failed on '%s': %k\n", #x, VkResultToString(_r));\
	} while (0)

String VkResultToString(VkResult r)
{
	switch (r)
	{
		case (VK_SUCCESS):
			return "VK_SUCCESS";
		case (VK_NOT_READY):
			return "VK_NOT_READY";
		case (VK_TIMEOUT):
			return "VK_TIMEOUT";
		case (VK_EVENT_SET):
			return "VK_EVENT_SET";
		case (VK_EVENT_RESET):
			return "VK_EVENT_RESET";
		case (VK_INCOMPLETE):
			return "VK_INCOMPLETE";
		case (VK_ERROR_OUT_OF_HOST_MEMORY):
			return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case (VK_ERROR_INITIALIZATION_FAILED):
			return "VK_ERROR_INITIALIZATION_FAILED";
		case (VK_ERROR_DEVICE_LOST):
			return "VK_ERROR_DEVICE_LOST";
		case (VK_ERROR_MEMORY_MAP_FAILED):
			return "VK_ERROR_MEMORY_MAP_FAILED";
		case (VK_ERROR_LAYER_NOT_PRESENT):
			return "VK_ERROR_LAYER_NOT_PRESENT";
		case (VK_ERROR_EXTENSION_NOT_PRESENT):
			return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case (VK_ERROR_FEATURE_NOT_PRESENT):
			return "VK_ERROR_FEATURE_NOT_PRESENT";
		case (VK_ERROR_INCOMPATIBLE_DRIVER):
			return "VK_ERROR_INCOMPATIBLE_DRIVER";
		case (VK_ERROR_TOO_MANY_OBJECTS):
			return "VK_ERROR_TOO_MANY_OBJECTS";
		case (VK_ERROR_FORMAT_NOT_SUPPORTED):
			return "VK_ERROR_FORMAT_NOT_SUPPORTED";
		case (VK_ERROR_FRAGMENTED_POOL):
			return "VK_ERROR_FRAGMENTED_POOL";
		case (VK_ERROR_OUT_OF_DEVICE_MEMORY):
			return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case (VK_ERROR_OUT_OF_POOL_MEMORY):
			return "VK_ERROR_OUT_OF_POOL_MEMORY";
		case (VK_ERROR_INVALID_EXTERNAL_HANDLE):
			return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
		case (VK_ERROR_SURFACE_LOST_KHR):
			return "VK_ERROR_SURFACE_LOST_KHR";
		case (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR):
			return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
		case (VK_SUBOPTIMAL_KHR):
			return "VK_SUBOPTIMAL_KHR";
		case (VK_ERROR_OUT_OF_DATE_KHR):
			return "VK_ERROR_OUT_OF_DATE_KHR";
		case (VK_ERROR_INCOMPATIBLE_DISPLAY_KHR):
			return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
		case (VK_ERROR_VALIDATION_FAILED_EXT):
			return "VK_ERROR_VALIDATION_FAILED_EXT";
		case (VK_ERROR_INVALID_SHADER_NV):
			return "VK_ERROR_INVALID_SHADER_NV";
		case (VK_ERROR_NOT_PERMITTED_EXT):
			return "VK_ERROR_NOT_PERMITTED_EXT";
		case (VK_RESULT_RANGE_SIZE):
			return "VK_RESULT_RANGE_SIZE";
		case (VK_RESULT_MAX_ENUM):
			return "VK_RESULT_MAX_ENUM";
		case (VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT):
			return "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
		case (VK_ERROR_FRAGMENTATION_EXT):
			return "VK_ERROR_FRAGMENTATION_EXT";
		case (VK_ERROR_INVALID_DEVICE_ADDRESS_EXT):
			return "VK_ERROR_INVALID_DEVICE_ADDRESS_EXT";
	}
	return "UnknownVkResultCode";
}

#define VK_EXPORTED_FUNCTION(name) PFN_##name name = NULL;
#define VK_GLOBAL_FUNCTION(name) PFN_##name name = NULL;
#define VK_INSTANCE_FUNCTION(name) PFN_##name name = NULL;
#define VK_DEVICE_FUNCTION(name) PFN_##name name = NULL;
#include "VulkanFunction.h"
#undef VK_EXPORTED_FUNCTION
#undef VK_GLOBAL_FUNCTION
#undef VK_INSTANCE_FUNCTION
#undef VK_DEVICE_FUNCTION

u32 VulkanDebugMessageCallback(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *userData)
{
	auto log = LogType{};
	auto sevStr = String{};
	switch (sev)
	{
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
	{
		log = INFO_LOG;
		sevStr = "Info";
	} break;
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
	{
		log = ERROR_LOG;
		sevStr = "Warning";
	} break;
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
	{
		log = ERROR_LOG;
		sevStr = "Error";
	} break;
	default:
	{
		log = ERROR_LOG;
		sevStr = "Unknown";
	};
	}
	auto typeStr = String{};
	switch (type)
	{
	case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:
	{
		typeStr = "General";
	} break;
	case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:
	{
		typeStr = "Validation";
	} break;
	case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT:
	{
		typeStr = "Performance";
	} break;
	default:
	{
		typeStr = "Unknown";
	};
	}
	if (sevStr == "Error")
	{
		Abort("Vulkan: %k: %k: %s", sevStr, typeStr, data->pMessage);
	}
	LogPrint(log, "GPU", "Vulkan: %k: %k: %s\n", sevStr, typeStr, data->pMessage);
	if (log == ERROR_LOG)
	{
		PrintStacktrace();
	}
    return 0;
}

Array<GPUMemoryHeapInfo> GPUMemoryHeapInfos()
{
	auto bp = VkPhysicalDeviceMemoryBudgetPropertiesEXT
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT
	};
	auto mp = VkPhysicalDeviceMemoryProperties2
	{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
		.pNext = &bp,
	};
	vkGetPhysicalDeviceMemoryProperties2(vkPhysicalDevice, &mp);
	auto heaps = NewArrayWithCount<GPUMemoryHeapInfo>(vkMemoryHeapCount);
	for (auto i = 0; i < vkMemoryHeapCount; i++)
	{
		heaps[i].usage = budgetProperties.heapUsage[i];
		heaps[i].budget = budgetProperties.heapBudget[i];
	}
	return heaps;
}

void PrintGPUMemoryHeapInfo()
{
	LogPrint(INFO_LOG, "GPU", "GPU memory info:\n");
	auto hi = GPUMemoryHeapInfos();
	for (auto i = 0; i < hi.count; i++)
	{
		LogPrint(
			INFO_LOG,
			"GPU",
			"	Heap: %d, Usage: %fmb, Total: %fmb\n",
			i,
			BytesToMegabytes(hi[i].usage),
			BytesToMegabytes(hi[i].budget));
	}
}

GfxCommandQueue GfxGetCommandQueue(GfxCommandQueueType queueType)
{
	auto queue = GfxCommandQueue{};
	switch (queueType)
	{
		case GFX_GRAPHICS_COMMAND_QUEUE:
		{
			vkGetDeviceQueue(vulkanGlobals.device, vulkanGlobals.graphicsQueueFamily, 0, &queue); // No return.
		} break;
		case GFX_TRANSFER_COMMAND_QUEUE:
		{
			vkGetDeviceQueue(vulkanGlobals.device, vulkanGlobals.transferQueueFamily, 0, &queue); // No return.
		} break;
		case GFX_COMPUTE_COMMAND_QUEUE:
		{
			vkGetDeviceQueue(vulkanGlobals.device, vulkanGlobals.computeQueueFamily, 0, &queue); // No return.
		} break;
		default:
		{
			Abort("Unknown queue type: %d.\n", queueType);
		} break;
	}
	return queue;
}

GPUBackendCommandBuffer CreateGPUBackendCommandBuffer(GfxCommandPool commandPool)
{
    VkCommandBufferAllocateInfo commandBufferAllocateInfo =
    {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
    };
	VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(vulkanGlobals.device, &commandBufferAllocateInfo, &commandBuffer);
	VkCommandBufferBeginInfo commandBufferBeginInfo =
	{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo);
	return commandBuffer;
}

void GfxSubmitCommandBuffers(GfxCommandQueue queue, GfxSubmitInfo &submitInfo, GfxFence fence)
{
	VkSubmitInfo vulkanSubmitInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = (u32)ArrayLength(submitInfo.waitSemaphores),
		.pWaitSemaphores = submitInfo.waitSemaphores.elements,
		.pWaitDstStageMask = submitInfo.waitStages.elements,
		.commandBufferCount = (u32)ArrayLength(submitInfo.commandBuffers),
		.pCommandBuffers = submitInfo.commandBuffers.elements,
		.signalSemaphoreCount = (u32)ArrayLength(submitInfo.signalSemaphores),
		.pSignalSemaphores = submitInfo.signalSemaphores.elements,
	};
	VK_CHECK(vkQueueSubmit(queue, 1, &vulkanSubmitInfo, fence));
}

void GfxFreeCommandBuffers(GfxCommandPool pool, s64 count, GPUBackendCommandBuffer *buffers)
{
	vkFreeCommandBuffers(vulkanGlobals.device, pool, count, buffers); // No return.
}

void GfxEndCommandBuffer(GPUCommandBuffer commandBuffer)
{
	VK_CHECK(vkEndCommandBuffer(commandBuffer.backend));
}

// @TODO: Store the queue family index in a GfxCommandQueue struct, and take that struct.
GfxCommandPool GfxCreateCommandPool(GfxCommandQueueType queueType)
{
	VkCommandPoolCreateInfo commandPoolCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		//.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	switch (queueType)
	{
	case GFX_GRAPHICS_COMMAND_QUEUE:
	{
		commandPoolCreateInfo.queueFamilyIndex = vulkanGlobals.graphicsQueueFamily;
	} break;
	case GFX_TRANSFER_COMMAND_QUEUE:
	{
		commandPoolCreateInfo.queueFamilyIndex = vulkanGlobals.transferQueueFamily;
	} break;
	case GFX_COMPUTE_COMMAND_QUEUE:
	{
		commandPoolCreateInfo.queueFamilyIndex = vulkanGlobals.computeQueueFamily;
	} break;
	default:
	{
		Abort("Invalid queue type %d.\n", queueType);
	} break;
	}
	VkCommandPool pool;
	VK_CHECK(vkCreateCommandPool(vulkanGlobals.device, &commandPoolCreateInfo, NULL, &pool));
	return pool;
}

void GfxResetCommandPool(GfxCommandPool pool)
{
	VK_CHECK(vkResetCommandPool(vulkanGlobals.device, pool, 0));
}

GfxBuffer GfxCreateBuffer(GfxSize size, GfxBufferUsageFlags usage)
{
	VkBufferCreateInfo bufferCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkBuffer buffer;
	VK_CHECK(vkCreateBuffer(vulkanGlobals.device, &bufferCreateInfo, NULL, &buffer));
	return buffer;
}

void GfxDestroyBuffer(GfxBuffer buffer)
{
	vkDestroyBuffer(vulkanGlobals.device, buffer, NULL);
}

void GfxRecordCopyBufferCommand(GPUCommandBuffer commandBuffer, GfxSize size, GfxBuffer source, GfxBuffer destination, GfxSize sourceOffset, GfxSize destinationOffset)
{
	VkBufferCopy bufferCopy =
	{
		.srcOffset = sourceOffset,
		.dstOffset = destinationOffset,
		.size = size,
	};
	vkCmdCopyBuffer(commandBuffer.backend, source, destination, 1, &bufferCopy);
}

GfxMemoryRequirements GfxGetBufferMemoryRequirements(GfxBuffer buffer)
{
	VkMemoryRequirements memoryRequirements;
	vkGetBufferMemoryRequirements(vulkanGlobals.device, buffer, &memoryRequirements);
	return memoryRequirements;
}

void GfxBindBufferMemory(GfxBuffer buffer, GfxMemory memory, s64 memoryOffset)
{
	VK_CHECK(vkBindBufferMemory(vulkanGlobals.device, buffer, memory, memoryOffset));
}

bool GfxAllocateMemory(GfxSize size, GfxMemoryType memoryType, GfxMemory *memory) // @TODO: Move memory up to the first parameter.
{
	PrintGPUMemoryInfo();
	LogPrint(INFO_LOG, "Allocating: %lu, %d\n", size, memoryType);
	auto index = vulkanGlobals.memoryTypeToMemoryIndex[memoryType];
	auto memoryAllocateInfo = VkMemoryAllocateInfo
	{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = size,
		.memoryTypeIndex = (u32)index,
	};
	auto result = vkAllocateMemory(vulkanGlobals.device, &memoryAllocateInfo, NULL, memory);
	if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY)
	{
		if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
		{
			LogPrint(INFO_LOG, "******** DEVICE\n");
		}
		else
		{
			LogPrint(INFO_LOG, "******** HOST\n");
		}

		return false;
	}
	VK_CHECK(result);
	return true;
}

void *GfxMapMemory(GfxMemory memory, GfxSize size, GfxSize offset)
{
	auto pointer = (void *){};
	VK_CHECK(vkMapMemory(vulkanGlobals.device, memory, offset, size, 0, &pointer));
	return pointer;
}

GfxShaderModule GfxCreateShaderModule(GfxShaderStage stage, String spirv)
{
	auto shaderModuleCreateInfo = VkShaderModuleCreateInfo
	{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = (u32)StringLength(spirv),
		.pCode = (u32 *)&spirv[0],
	};
	auto module = VkShaderModule{};
	VK_CHECK(vkCreateShaderModule(vulkanGlobals.device, &shaderModuleCreateInfo, NULL, &module));
	return module;
}

GfxFence GfxCreateFence(bool startSignalled)
{
	auto fenceCreateInfo = VkFenceCreateInfo
	{
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = 0,
	};
	if (startSignalled)
	{
		fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	}
	auto fence = VkFence{};
	VK_CHECK(vkCreateFence(vulkanGlobals.device, &fenceCreateInfo, NULL, &fence));
	return fence;
}

bool GfxWasFenceSignalled(GfxFence fence)
{
	auto result = vkGetFenceStatus(vulkanGlobals.device, fence);
	if (result == VK_SUCCESS)
	{
		return true;
	}
	if (result == VK_NOT_READY)
	{
		return false;
	}
	VK_CHECK(result);
	return false;
}

void GfxWaitForFences(u32 count, GfxFence *fences, bool waitForAllFences, u64 timeout)
{
	VK_CHECK(vkWaitForFences(vulkanGlobals.device, count, fences, waitForAllFences, timeout));
}

void GfxResetFences(u32 count, GfxFence *fences)
{
	VK_CHECK(vkResetFences(vulkanGlobals.device, count, fences));
}

GfxSemaphore GfxCreateSemaphore()
{
	VkSemaphoreCreateInfo semaphoreCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	VkSemaphore semaphore;
	VK_CHECK(vkCreateSemaphore(vulkanGlobals.device, &semaphoreCreateInfo, NULL, &semaphore));
	return semaphore;
}

GfxSwapchain GfxCreateSwapchain()
{
	VkSurfaceCapabilitiesKHR surfaceCapabilities;
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vulkanGlobals.physicalDevice, vulkanGlobals.surface, &surfaceCapabilities));
	VkExtent2D swapchainImageExtent;
	if (surfaceCapabilities.currentExtent.width == U32_MAX && surfaceCapabilities.currentExtent.height == U32_MAX) // Indicates Vulkan will accept any extent dimension.
	{
		swapchainImageExtent.width = GetRenderWidth();
		swapchainImageExtent.height = GetRenderHeight();
	}
	else
	{
		swapchainImageExtent.width = Maximum(surfaceCapabilities.minImageExtent.width, Minimum(surfaceCapabilities.maxImageExtent.width, GetRenderWidth()));
		swapchainImageExtent.height = Maximum(surfaceCapabilities.minImageExtent.height, Minimum(surfaceCapabilities.maxImageExtent.height, GetRenderHeight()));
	}
	// @TODO: Why is this a problem?
	if (swapchainImageExtent.width != GetRenderWidth() && swapchainImageExtent.height != GetRenderHeight())
	{
		Abort("swapchain image dimensions do not match the window dimensions: swapchain %ux%u, window %ux%u\n", swapchainImageExtent.width, swapchainImageExtent.height, GetRenderWidth(), GetRenderHeight());
	}
	u32 desiredSwapchainImageCount = surfaceCapabilities.minImageCount + 1;
	if (surfaceCapabilities.maxImageCount > 0 && (desiredSwapchainImageCount > surfaceCapabilities.maxImageCount))
	{
		desiredSwapchainImageCount = surfaceCapabilities.maxImageCount;
	}
	VkSwapchainCreateInfoKHR swapchainCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = vulkanGlobals.surface,
		.minImageCount = desiredSwapchainImageCount,
		.imageFormat = vulkanGlobals.surfaceFormat.format,
		.imageColorSpace  = vulkanGlobals.surfaceFormat.colorSpace,
		.imageExtent = swapchainImageExtent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = surfaceCapabilities.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = vulkanGlobals.presentMode,
		.clipped = 1,
		.oldSwapchain = NULL,
	};
	u32 queueFamilyIndices[] =
	{
		vulkanGlobals.graphicsQueueFamily,
		vulkanGlobals.presentQueueFamily,
	};
	if (vulkanGlobals.graphicsQueueFamily != vulkanGlobals.presentQueueFamily)
	{
		swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		swapchainCreateInfo.queueFamilyIndexCount = CArrayCount(queueFamilyIndices);
		swapchainCreateInfo.pQueueFamilyIndices = queueFamilyIndices;
	}
	else
	{
		swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}
	VkSwapchainKHR swapchain;
	VK_CHECK(vkCreateSwapchainKHR(vulkanGlobals.device, &swapchainCreateInfo, NULL, &swapchain));
	return swapchain;
}

void GetGPUSwapchainImages(GPUSwapchain sc, Array<GPUImage> *out)
{
	auto numImages = u32{};
	VK_CHECK(vkGetSwapchainImagesKHR(vulkanGlobals.device, sc, &numImages, NULL));
	auto internImgs = NewArrayWithCount<GPUInternalImage>(numImages);
	VK_CHECK(vkGetSwapchainImagesKHR(vulkanGlobals.device, sc, &numImages, &internImgs[0]));
	out->Resize(numImages);
	for (auto i = 0; i < numImages; i++)
	{
		(*out)[i].internal = internImgs[i];
		(*out)[i].memory = NULL;
	}
	internImgs.Free();
}

u32 GfxAcquireNextSwapchainImage(GfxSwapchain swapchain, GfxSemaphore semaphore)
{
	auto swapchainImageIndex = u32{};
	VK_CHECK(vkAcquireNextImageKHR(vulkanGlobals.device, swapchain, UINT64_MAX, semaphore, NULL, &swapchainImageIndex));
	return swapchainImageIndex;
}

VkFormat GfxGetSurfaceFormat()
{
	return vulkanGlobals.surfaceFormat.format;
}

#if 0
void VulkanGetSwapchainImageViews(GfxSwapchain swapchain, u32 count, GfxImageView *imageViews)
{
	VkImage images[count]; // @TODO: Use a real array.
	VK_CHECK(vkGetSwapchainImagesKHR(vulkanGlobals.device, swapchain, &count, images));
	for (auto i = 0; i < count; i++)
	{
		VkImageViewCreateInfo imageViewCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = images[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = vulkanGlobals.surfaceFormat.format,
			.components =
			{
				.r = VK_COMPONENT_SWIZZLE_IDENTITY,
				.g = VK_COMPONENT_SWIZZLE_IDENTITY,
				.b = VK_COMPONENT_SWIZZLE_IDENTITY,
				.a = VK_COMPONENT_SWIZZLE_IDENTITY,
			},
			.subresourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		VK_CHECK(vkCreateImageView(vulkanGlobals.device, &imageViewCreateInfo, NULL, &imageViews[i]));
	}
}
#endif

// @TODO: Better abstraction for render passes.

typedef VkAttachmentDescription GFX_Framebuffer_Attachment_Description;
typedef VkSubpassDescription GFX_Subpass_Description;
typedef VkSubpassDependency GFX_Subpass_Dependency;

typedef struct GFX_Attachment_Description {
} GFX_Attachment_Description;

/*
typedef struct xRender_Pass {
	u32 subpass_dependency_count;
	GFX_Subpass_Dependency *subpass_dependencies;
	u32 subpass_count;
	GFX_Subpass_Description *subpass_descriptions;
	u32 attachment_count;
	GFX_Attachment_Description *attachment_descriptions;
} xRender_Pass;
*/

#if 0
typedef struct GFX_Render_Graph {
	u32 render_pass_count;
	VkRenderPass *render_passes;
} GFX_Render_Graph;

GFX_Render_Graph GFX_Compile_Render_Graph(Render_API_Context *context, Render_Graph_Description *render_graph_description) {
	GFX_Render_Graph render_graph = {
		.render_pass_count = 2,
		.render_passes = (VkRenderPass *)malloc(2 * sizeof(VkRenderPass)), // @TODO
	};
	{
		VkSubpassDependency subpass_dependencies[] = {
			{
				.srcSubpass      = VK_SUBPASS_EXTERNAL,
				.dstSubpass      = 0,
				.srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				.dstStageMask    = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
				.srcAccessMask   = VK_ACCESS_SHADER_READ_BIT,
				.dstAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
			},
			{
				.srcSubpass      = 0,
				.dstSubpass      = VK_SUBPASS_EXTERNAL,
				.srcStageMask    = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				.dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				.srcAccessMask   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
				.dstAccessMask   = VK_ACCESS_SHADER_READ_BIT,
				.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
			}
		};
		VkAttachmentDescription AttachmentDescription = {
			.format = (VkFormat)SHADOW_MAP_FORMAT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		};
		VkRenderPassCreateInfo render_pass_create_info = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = AttachmentDescription,
			.subpassCount = 1,
			.pSubpasses      = &(VkSubpassDescription){
				.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
				.colorAttachmentCount    = 0,
				.pDepthStencilAttachment = &(VkAttachmentReference){
					.attachment = 0,
					.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				},
			},
			.dependencyCount = CArrayCount(subpass_dependencies),
			.pDependencies   = subpass_dependencies,
		};
		VK_CHECK(vkCreateRenderPass(context->device, &render_pass_create_info, NULL, &render_graph.render_passes[0]));
	}
	{
		VkAttachmentDescription attachments[] = {
			{
				.format         = context->window_surface_format.format,
				.samples        = VK_SAMPLE_COUNT_1_BIT,
				.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
				.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
				.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			},
			{
				.format         = VK_FORMAT_D32_SFLOAT_S8_UINT,
				.samples        = VK_SAMPLE_COUNT_1_BIT,
				.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
				.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			},
		};
		VkRenderPassCreateInfo render_pass_create_info = {
			.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.attachmentCount = CArrayCount(attachments),
			.pAttachments    = attachments,
			.subpassCount    = 1,
			.pSubpasses      = &(VkSubpassDescription){
				.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
				.colorAttachmentCount = 1,
				.pColorAttachments    = &(VkAttachmentReference){
					.attachment = 0,
					.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				},
				.pDepthStencilAttachment = &(VkAttachmentReference){
					.attachment = 1,
					.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
				},
			},
			.dependencyCount = 1,
			.pDependencies   = &(VkSubpassDependency){
				.srcSubpass    = VK_SUBPASS_EXTERNAL,
				.dstSubpass    = 0,
				.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				.srcAccessMask = 0,
				.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			},
		};
		VK_CHECK(vkCreateRenderPass(context->device, &render_pass_create_info, NULL, &render_graph.render_passes[1]));
	}
	return render_graph;
}
#endif

GfxDescriptorPool GfxCreateDescriptorPool()
{
	VkDescriptorPoolSize descriptorPoolSize =
	{
		.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = 100, // @TODO
	};
	VkDescriptorPoolCreateInfo descriptorPoolCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT,
		.maxSets = 100, // @TODO
		.poolSizeCount = 1,
		.pPoolSizes = &descriptorPoolSize,
	};
	VkDescriptorPool descriptorPool;
	VK_CHECK(vkCreateDescriptorPool(vulkanGlobals.device, &descriptorPoolCreateInfo, NULL, &descriptorPool));
	return descriptorPool;
}

GfxDescriptorSetLayout GfxCreateDescriptorSetLayout(u32 bindingCount, DescriptorSetBindingInfo *bindingInfos)
{
	VkDescriptorSetLayoutBinding bindings[bindingCount];
	for (auto i = 0; i < bindingCount; i++)
	{
		bindings[i] =
		VkDescriptorSetLayoutBinding
		{
			.binding = bindingInfos[i].binding,
			.descriptorType = bindingInfos[i].descriptorType,
			.descriptorCount = bindingInfos[i].descriptorCount,
			.stageFlags = bindingInfos[i].stageFlags,
			.pImmutableSamplers = NULL,
		};
	};
	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT,
		.bindingCount = bindingCount,
		.pBindings = bindings,
	};
	VkDescriptorSetLayout layout;
	VK_CHECK(vkCreateDescriptorSetLayout(vulkanGlobals.device, &descriptorSetLayoutCreateInfo, NULL, &layout));
	return layout;
}

void GfxCreateDescriptorSets(GfxDescriptorPool pool, GfxDescriptorSetLayout layout, u32 setCount, GfxDescriptorSet *sets)
{
	VkDescriptorSetLayout layouts[setCount];
	for (auto i = 0; i < setCount; i++)
	{
		layouts[i] = layout;
	}
	VkDescriptorSetAllocateInfo descriptorSetAllocateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool,
		.descriptorSetCount = setCount,
		.pSetLayouts = layouts,
	};
	VK_CHECK(vkAllocateDescriptorSets(vulkanGlobals.device, &descriptorSetAllocateInfo, sets));
}

void GfxUpdateDescriptorSets(GfxDescriptorSet set, GfxBuffer buffer, GfxDescriptorType descriptorType, u32 binding, GfxSize offset, GfxSize range)
{
	VkDescriptorBufferInfo bufferInfo =
	{
		.buffer = buffer,
		.offset = 0,
		.range = range,
	};
	VkWriteDescriptorSet descriptorWrite =
	{
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set,
		.dstBinding = binding,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = descriptorType,
		.pBufferInfo = &bufferInfo,
	};
	vkUpdateDescriptorSets(vulkanGlobals.device, 1, &descriptorWrite, 0, NULL);
}

void GfxRecordBindDescriptorSetsCommand(GPUCommandBuffer commandBuffer, GfxPipelineBindPoint pipelineBindPoint, GfxPipelineLayout pipelineLayout, s64 firstSetNumber, s64 setCount, GfxDescriptorSet *sets)
{
	vkCmdBindDescriptorSets(commandBuffer.backend, pipelineBindPoint, pipelineLayout, firstSetNumber, setCount, sets, 0, NULL);
}

#if 0
void Render_API_Update_Descriptor_Sets(Render_API_Context *context, s32 swapchain_image_index, GFX_Descriptor_Set set, GFX_Buffer buffer) {
	VkDescriptorBufferInfo buffer_info = {
		.buffer = buffer,
		.offset = (u64)(swapchain_image_index * 0x100),
		.range = sizeof(M4),
	};
	VkWriteDescriptorSet descriptor_write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set,
		.dstBinding = 0,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &buffer_info,
	};
	vkUpdateDescriptorSets(context->device, 1, &descriptor_write, 0, NULL);
}

GFX_Descriptor_Set Render_API_Create_Shader_Descriptor_Sets(Render_API_Context *context, GFX_Shader_ID shader_id) {
	VkWriteDescriptorSet descriptor_writes[10]; // @TODO
	s32 write_count = 0;
	uniform_buffer = Create_GFX_Device_Buffer(context, sizeof(M4) * context->swapchain_image_count + 0x100 * context->swapchain_image_count, GFX_UNIFORM_BUFFER | GFX_TRANSFER_DESTINATION_BUFFER);
	VkDescriptorBufferInfo buffer_infos[context->swapchain_image_count];
	s32 buffer_info_count = 0;
	for (s32 i = 0; i < context->swapchain_image_count; i++) {
		buffer_infos[buffer_info_count] = (VkDescriptorBufferInfo){
			.buffer = uniform_buffer,
			.offset = i * 0x100,
			.range  = sizeof(M4),
		};
		descriptor_writes[write_count++] = (VkWriteDescriptorSet){
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet          = context->descriptor_sets.rusted_iron_vertex_bind_per_object_update_immediate[i],
			.dstBinding      = 0,
			.dstArrayElement = 0,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.pBufferInfo     = &buffer_infos[buffer_info_count],
		};
		buffer_info_count++;
	}
	vkUpdateDescriptorSets(context->api_context.device, write_count, descriptor_writes, 0, NULL);
}
#endif

typedef struct GFX_Image_Descriptor_Write {
	GfxImageLayout layout;
	GfxImageView view;
	GfxSampler sampler;
} GFX_Image_Descriptor_Write;

typedef struct GFX_Buffer_Descriptor_Write {
	GfxBuffer buffer;
	u32 offset;
	u32 range;
} GFX_Buffer_Descriptor_Write;

typedef struct GFX_Descriptor_Write {
	GFX_Image_Descriptor_Write *image;
	GFX_Buffer_Descriptor_Write *buffer;
} GFX_Descriptor_Write;

typedef struct GFX_Descriptor_Description {
	u32 count;
	u32 binding;
	GfxDescriptorType type;
	GfxShaderStage stage;
	GFX_Descriptor_Write write; 
	u32 flags;
} GFX_Descriptor_Description;

#if 0
void GFX_Create_Descriptor_Sets(Render_API_Context *context, s32 count, GFX_Descriptor_Pool descriptor_pool, GFX_Descriptor_Set *sets) {
	VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = descriptor_pool,
		.descriptorSetCount = count,
		.pSetLayouts = &descriptor_set.layout,
	};
	VK_CHECK(vkAllocateDescriptorSets(context->device, &descriptor_set_allocate_info, &descriptor_set.descriptor_set));
}

GFX_Descriptor_Set GFX_Create_Descriptor_Set(Render_API_Context *context, u32 descriptor_count, GFX_Descriptor_Description *descriptor_descriptions, u32 flags, VkDescriptorPool descriptor_pool) {
	GFX_Descriptor_Set descriptor_set;
	VkDescriptorSetLayoutBinding bindings[descriptor_count];
	for (u32 i = 0; i < descriptor_count; i++) {
		bindings[i] = (VkDescriptorSetLayoutBinding){
			.binding = descriptor_descriptions[i].binding,
			.descriptorType = descriptor_descriptions[i].type,
			.descriptorCount = descriptor_descriptions[i].count,
			.stageFlags = descriptor_descriptions[i].stage_flags,
			.pImmutableSamplers = NULL,
		};
	}
	VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = descriptor_count,
		.pBindings = bindings,
		.flags = flags,
	};
	VK_CHECK(vkCreateDescriptorSetLayout(context->device, &descriptor_set_layout_create_info, NULL, &descriptor_set.layout));
	VkDescriptorSetAllocateInfo descriptor_set_allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &descriptor_set.layout,
	};
	VK_CHECK(vkAllocateDescriptorSets(context->device, &descriptor_set_allocate_info, &descriptor_set.descriptor_set));

	VkWriteDescriptorSet descriptor_writes[descriptor_count];
	VkDescriptorImageInfo descriptor_image_infos[descriptor_count];
	VkDescriptorBufferInfo descriptor_buffer_infos[descriptor_count];
	for (u32 i = 0; i < descriptor_count; i++) {
		descriptor_writes[i] = (VkWriteDescriptorSet){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = descriptor_set.descriptor_set,
			.dstBinding = descriptor_descriptions[i].binding,
			.dstArrayElement = 0,
			.descriptorType = descriptor_descriptions[i].type,
			.descriptorCount = 1,
		};
		Assert(descriptor_descriptions[i].write.image || descriptor_descriptions[i].write.buffer);
		if (descriptor_descriptions[i].write.image) {
			 descriptor_image_infos[i] = (VkDescriptorImageInfo){
				.imageLayout = descriptor_descriptions[i].write.image->layout,
				.imageView = descriptor_descriptions[i].write.image->view,
				.sampler = descriptor_descriptions[i].write.image->sampler,
			};
			descriptor_writes[i].pImageInfo = &descriptor_image_infos[i];
		} else {
			descriptor_buffer_infos[i] = (VkDescriptorBufferInfo){
				.buffer = descriptor_descriptions[i].write.buffer->buffer,
				.offset = descriptor_descriptions[i].write.buffer->offset,
				.range = descriptor_descriptions[i].write.buffer->range,
			};
			descriptor_writes[i].pBufferInfo = &descriptor_buffer_infos[i];
		}
	}
	vkUpdateDescriptorSets(context->device, descriptor_count, descriptor_writes, 0, NULL); // No return.
	return descriptor_set;
}
#endif

GfxPipelineLayout GfxCreatePipelineLayout(u32 layoutCount, GfxDescriptorSetLayout *layouts)
{
	VkPipelineLayoutCreateInfo layoutCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = layoutCount,
		.pSetLayouts = layouts,
		.pushConstantRangeCount = 0,
	};
	/* @TODO
	VkPushConstantRange push_constant_ranges[pipeline_description.push_constant_count];
	for (auto i = 0; i < pipeline_description.push_constant_count; i++) {
		push_constant_ranges[i] = (VkPushConstantRange){
			.stageFlags = pipeline_description.push_constant_descriptions[i].shader_stage,
			.offset = pipeline_description.push_constant_descriptions[i].offset,
			.size = pipeline_description.push_constant_descriptions[i].offset,
		};
	}
	layoutCreateInfo.pPushConstantRanges = push_constant_ranges;
	*/
	GfxPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(vulkanGlobals.device, &layoutCreateInfo, NULL, &layout));
	return layout;
}

struct GfxPushConstantDescription
{
	u32 offset;
	u32 size;
	GfxShaderStage shader_stage;
};

// @TODO: Fix formatting.
GfxPipeline GfxCreatePipeline(GfxPipelineDescription pipeline_description)
{
	VkPipelineInputAssemblyStateCreateInfo input_assembly_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = (VkPrimitiveTopology)pipeline_description.topology,
		.primitiveRestartEnable = VK_FALSE,
	};
	VkViewport viewport = {
		.x = 0.0f,
		.y = 0.0f,
		.width = pipeline_description.viewport_width,
		.height = pipeline_description.viewport_height,
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	VkRect2D scissor = {
		.offset = {0, 0},
		.extent = (VkExtent2D){pipeline_description.scissor_width, pipeline_description.scissor_height},
	};
	VkPipelineViewportStateCreateInfo viewport_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports = &viewport,
		.scissorCount = 1,
		.pScissors = &scissor,
	};
	VkPipelineRasterizationStateCreateInfo rasterization_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.depthClampEnable = VK_FALSE,
		.rasterizerDiscardEnable = VK_FALSE,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.depthBiasEnable = pipeline_description.enable_depth_bias,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		.sampleShadingEnable = VK_FALSE,
	};
	VkPipelineDepthStencilStateCreateInfo depth_stencil_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = (VkCompareOp)pipeline_description.depth_compare_operation,
		.depthBoundsTestEnable = VK_FALSE,
		.stencilTestEnable = VK_FALSE,
	};
	VkPipelineColorBlendAttachmentState color_blend_attachment_states[pipeline_description.framebuffer_attachment_color_blend_count];
	for (auto i = 0; i < pipeline_description.framebuffer_attachment_color_blend_count; i++) {
		color_blend_attachment_states[i] = (VkPipelineColorBlendAttachmentState){
			.blendEnable = pipeline_description.framebuffer_attachment_color_blend_descriptions[i].enable_blend,
			.srcColorBlendFactor = (VkBlendFactor)pipeline_description.framebuffer_attachment_color_blend_descriptions[i].source_color_blend_factor,
			.dstColorBlendFactor = (VkBlendFactor)pipeline_description.framebuffer_attachment_color_blend_descriptions[i].destination_color_blend_factor,
			.colorBlendOp = (VkBlendOp)pipeline_description.framebuffer_attachment_color_blend_descriptions[i].color_blend_operation,
			.srcAlphaBlendFactor = (VkBlendFactor)pipeline_description.framebuffer_attachment_color_blend_descriptions[i].source_alpha_blend_factor,
			.dstAlphaBlendFactor = (VkBlendFactor)pipeline_description.framebuffer_attachment_color_blend_descriptions[i].destination_alpha_blend_factor,
			.alphaBlendOp = (VkBlendOp)pipeline_description.framebuffer_attachment_color_blend_descriptions[i].alpha_blend_operation,
			.colorWriteMask = pipeline_description.framebuffer_attachment_color_blend_descriptions[i].color_write_mask,
		};
	}
	VkPipelineColorBlendStateCreateInfo color_blend_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.logicOpEnable = VK_FALSE,
		.logicOp = VK_LOGIC_OP_COPY,
		.attachmentCount = pipeline_description.framebuffer_attachment_color_blend_count,
		.pAttachments = color_blend_attachment_states,
		.blendConstants = {},
	};
	VkVertexInputAttributeDescription vertex_input_attribute_descriptions[pipeline_description.vertex_input_attribute_count];
	for (auto i = 0; i < pipeline_description.vertex_input_attribute_count; i++) {
		vertex_input_attribute_descriptions[i] = (VkVertexInputAttributeDescription){
			.location = pipeline_description.vertex_input_attribute_descriptions[i].location,
			.binding = pipeline_description.vertex_input_attribute_descriptions[i].binding,
			.format = (VkFormat)pipeline_description.vertex_input_attribute_descriptions[i].format,
			.offset = pipeline_description.vertex_input_attribute_descriptions[i].offset,
		};
	}
	VkVertexInputBindingDescription vertex_input_binding_descriptions[pipeline_description.vertex_input_binding_count];
	for (auto i = 0; i < pipeline_description.vertex_input_binding_count; i++) {
		vertex_input_binding_descriptions[i] = (VkVertexInputBindingDescription){
			.binding = pipeline_description.vertex_input_binding_descriptions[i].binding,
			.stride = pipeline_description.vertex_input_binding_descriptions[i].stride,
			.inputRate = (VkVertexInputRate)pipeline_description.vertex_input_binding_descriptions[i].input_rate,
		};
	}
	VkPipelineVertexInputStateCreateInfo vertex_input_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = pipeline_description.vertex_input_binding_count,
		.pVertexBindingDescriptions = vertex_input_binding_descriptions,
		.vertexAttributeDescriptionCount = pipeline_description.vertex_input_attribute_count,
		.pVertexAttributeDescriptions = vertex_input_attribute_descriptions,
	};
	VkPipelineDynamicStateCreateInfo dynamic_state_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = pipeline_description.dynamic_state_count,
		.pDynamicStates = (VkDynamicState *)pipeline_description.dynamic_states,
	};
	Assert(ArrayLength(pipeline_description.shaderStages) == ArrayLength(pipeline_description.shaderModules));
	VkPipelineShaderStageCreateInfo shader_stage_create_infos[ArrayLength(pipeline_description.shaderStages)];
	for (auto i = 0; i < ArrayLength(pipeline_description.shaderStages); i++)
	{
		shader_stage_create_infos[i] =
		(VkPipelineShaderStageCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = pipeline_description.shaderStages[i],
			.module = pipeline_description.shaderModules[i],
			.pName = "main",
		};
	}
	VkGraphicsPipelineCreateInfo graphics_pipeline_create_info =
	{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = (u32)ArrayLength(pipeline_description.shaderStages),
		.pStages = shader_stage_create_infos,
		.pVertexInputState = &vertex_input_state_create_info,
		.pInputAssemblyState = &input_assembly_create_info,
		.pViewportState = &viewport_state_create_info,
		.pRasterizationState = &rasterization_state_create_info,
		.pMultisampleState = &multisample_state_create_info,
		.pDepthStencilState = &depth_stencil_state_create_info,
		.pColorBlendState = &color_blend_state_create_info,
		.pDynamicState = &dynamic_state_create_info,
		.layout = pipeline_description.layout,
		.renderPass = pipeline_description.render_pass,
		.subpass = 0,
		.basePipelineHandle = VK_NULL_HANDLE,
		.basePipelineIndex = -1,
	};
	VkPipeline pipeline;
	VK_CHECK(vkCreateGraphicsPipelines(vulkanGlobals.device, NULL, 1, &graphics_pipeline_create_info, NULL, &pipeline));
	return pipeline;
}

GfxFramebuffer GfxCreateFramebuffer(GfxRenderPass renderPass, u32 width, u32 height, u32 attachmentCount, GfxImageView *attachments)
{
	VkFramebufferCreateInfo framebufferCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = renderPass,
		.attachmentCount = attachmentCount,
		.pAttachments = attachments,
		.width = width,
		.height = height,
		.layers = 1,
	};
	VkFramebuffer framebuffer;
	VK_CHECK(vkCreateFramebuffer(vulkanGlobals.device, &framebufferCreateInfo, NULL, &framebuffer));
	return framebuffer;
}

GfxMemoryRequirements GfxGetImageMemoryRequirements(GfxImage image)
{
	VkMemoryRequirements imageMemoryRequirements;
	vkGetImageMemoryRequirements(vulkanGlobals.device, image, &imageMemoryRequirements);
	return imageMemoryRequirements;
}

GfxImage GfxCreateImage(u32 width, u32 height, GfxFormat format, GfxImageLayout initialLayout, GfxImageUsageFlags usage, GfxSampleCount sampleCount)
{
	VkImageCreateInfo imageCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = (VkFormat)format,
		.extent =
		{
			.width = width,
			.height = height,
			.depth = 1,
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = sampleCount,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = (VkImageLayout)initialLayout,
	};
	GfxImage image;
	VK_CHECK(vkCreateImage(vulkanGlobals.device, &imageCreateInfo, NULL, &image));
	return image;
}

void GfxBindImageMemory(GfxImage image, GfxMemory memory, s64 offset)
{
	VK_CHECK(vkBindImageMemory(vulkanGlobals.device, image, memory, offset));
}

GfxImageView GfxCreateImageView(GfxImage image, GfxImageViewType viewType, GfxFormat format, GfxSwizzleMapping swizzleMapping, GfxImageSubresourceRange subresourceRange)
{
	VkImageViewCreateInfo imageViewCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = viewType, //VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.components = 
		{
			.r = swizzleMapping.r,
			.g = swizzleMapping.g,
			.b = swizzleMapping.b,
			.a = swizzleMapping.a,
		},
		.subresourceRange = subresourceRange,
	};
	/*
	if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
	{
		imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	}
	else
	{
		imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	*/
	VkImageView imageView;
	VK_CHECK(vkCreateImageView(vulkanGlobals.device, &imageViewCreateInfo, NULL, &imageView));
	return imageView;
}

void GfxTransitionImageLayout(GPUCommandBuffer commandBuffer, GfxImage image, GfxFormat format, GfxImageLayout oldLayout, GfxImageLayout newLayout)
{
	auto barrier = VkImageMemoryBarrier
	{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = (VkImageLayout)oldLayout,
		.newLayout = (VkImageLayout)newLayout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange =
		{
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};
	if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT)
		{
			barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	}
	else
	{
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	auto sourceStage = VkPipelineStageFlags{};
	auto destinationStage = VkPipelineStageFlags{};
	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}
	else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	{
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	}
	else
	{
		Abort("Unsupported Vulkan image layout transition.");
	}
	vkCmdPipelineBarrier(commandBuffer.backend, sourceStage, destinationStage, 0, 0, NULL, 0, NULL, 1, &barrier); // No return.
}

void GfxRecordCopyBufferToImageCommand(GPUCommandBuffer commandBuffer, GfxBuffer buffer, GfxImage image, u32 imageWidth, u32 imageHeight)
{
	VkBufferImageCopy region =
	{
		.bufferOffset = 0,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource =
		{
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.imageOffset = {},
		.imageExtent =
		{
			imageWidth,
			imageHeight,
			1,
		},
	};
	vkCmdCopyBufferToImage(commandBuffer.backend, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void GfxPresentSwapchainImage(GfxSwapchain swapchain, u32 swapchainImageIndex, Array<GfxSemaphore> waitSemaphores)
{
	if (vulkanGlobals.graphicsQueueFamily != vulkanGlobals.presentQueueFamily)
	{
		// If we are using separate queues, change image ownership to the present queue before
		// presenting, waiting for the draw complete semaphore and signalling the ownership released
		// semaphore when finished.
		auto submitInfo = VkSubmitInfo
		{
			.waitSemaphoreCount = (u32)waitSemaphores.count,
			.pWaitSemaphores = waitSemaphores.elements,
			.commandBufferCount = 1,
			.pCommandBuffers = &vulkanGlobals.changeImageOwnershipFromGraphicsToPresentQueueCommands[swapchainImageIndex],
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &vulkanGlobals.imageOwnershipSemaphores[GetFrameIndex()],
		};
		VK_CHECK(vkQueueSubmit(vulkanGlobals.presentQueue, 1, &submitInfo, VK_NULL_HANDLE));
	}

	auto presentInfo = VkPresentInfoKHR
	{
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.swapchainCount = 1,
		.pSwapchains = &swapchain,
		.pImageIndices = &swapchainImageIndex,
	};
	if (vulkanGlobals.graphicsQueueFamily != vulkanGlobals.presentQueueFamily)
	{
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &vulkanGlobals.imageOwnershipSemaphores[GetFrameIndex()];
	}
	else
	{
		presentInfo.waitSemaphoreCount = (u32)waitSemaphores.count;
		presentInfo.pWaitSemaphores = waitSemaphores.elements;
	}
	VK_CHECK(vkQueuePresentKHR(vulkanGlobals.presentQueue, &presentInfo));
}

#if 0
void _Vulkan_Transition_Image_Layout(VkCommandBuffer command_buffer, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkFormat format) {
	VkImageMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = old_layout,
		.newLayout = new_layout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = {
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};
	if (new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT) {
			barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
	} else {
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	VkPipelineStageFlags source_stage;
	VkPipelineStageFlags destination_stage;
	if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	} else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	} else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destination_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	} else {
		Abort("Unsupported Vulkan layout transition");
	}
	vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}
#endif

GfxRenderPass TEMPORARY_Render_API_Create_Render_Pass()
{
	VkAttachmentDescription attachments[] =
	{
		{
			.format = vulkanGlobals.surfaceFormat.format,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		},
		{
			.format = VK_FORMAT_D32_SFLOAT_S8_UINT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		},
	};
	VkAttachmentReference colorAttachments[] =
	{
		{
			.attachment = 0,
			.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		},
	};
	VkAttachmentReference stencilAttachment =
	{
		.attachment = 1,
		.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpassDescriptions[] =
	{
		{
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = CArrayCount(colorAttachments),
			.pColorAttachments = colorAttachments,
			.pDepthStencilAttachment = &stencilAttachment,
		},
	};
	VkSubpassDependency subpassDependencies[] =
	{
		{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		},
	};
	VkRenderPassCreateInfo renderPassCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = CArrayCount(attachments),
		.pAttachments = attachments,
		.subpassCount = CArrayCount(subpassDescriptions),
		.pSubpasses = subpassDescriptions,
		.dependencyCount = CArrayCount(subpassDependencies),
		.pDependencies = subpassDependencies,
	};
	VkRenderPass renderPass;
	VK_CHECK(vkCreateRenderPass(vulkanGlobals.device, &renderPassCreateInfo, NULL, &renderPass));
	return renderPass;
}

typedef struct GFX_Sampler_Description {
	GfxSamplerFilter filter;
	GfxSamplerAddressMode address_mode_u;
	GfxSamplerAddressMode address_mode_v;
	GfxSamplerAddressMode address_mode_w;
	f32 mipmap_lod_bias;
	u32 max_anisotropy;
	f32 min_lod;
	f32 max_lod;
	GfxBorderColor border_color;
} GFX_Sampler_Description;

VkSampler Vulkan_Create_Sampler(Render_API_Context *context, VkSamplerCreateInfo *sampler_create_info) {
	VkSampler sampler;
	VK_CHECK(vkCreateSampler(context->device, sampler_create_info, NULL, &sampler));
	return sampler;
#if 0
	VkSamplerCreateInfo sampler_create_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = sampler_description.filter.vulkan.mag,
		.minFilter = sampler_description.filter.vulkan.min,
		.mipmapMode = sampler_description.filter.vulkan.mipmap,
		.addressModeU = sampler_description.address_mode_u,
		.addressModeV = sampler_description.address_mode_v,
		.addressModeW = sampler_description.address_mode_w,
		.mipLodBias = sampler_description.mipmap_lod_bias,
		.maxAnisotropy = sampler_description.max_anisotropy,
		.minLod = sampler_description.min_lod,
		.maxLod = sampler_description.max_lod,
		.borderColor = sampler_description.border_color,
	};
	VkSampler sampler;
	VK_CHECK(vkCreateSampler(context->vulkan.device, &sampler_create_info, NULL, &sampler));
	return sampler;
#endif
}

void GfxRecordBeginRenderPassCommand(GPUCommandBuffer commandBuffer, GfxRenderPass renderPass, GfxFramebuffer framebuffer)
{
	VkClearValue clearColor = {{{0.04f, 0.19f, 0.34f, 1.0f}}};
	VkClearValue clearDepthStencil = {{{1.0f, 0.0f}}};
	VkClearValue clearValues[] =
	{
		clearColor,
		clearDepthStencil,
	};
	VkRenderPassBeginInfo renderPassBeginInfo =
	{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = renderPass,
		.framebuffer = framebuffer,
		.renderArea =
		{
			.offset = {0, 0},
			.extent = {(u32)GetRenderWidth(), (u32)GetRenderHeight()},
		},
		.clearValueCount = CArrayCount(clearValues),
		.pClearValues = clearValues,
	};
	vkCmdBeginRenderPass(commandBuffer.backend, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
}

void GfxRecordEndRenderPassCommand(GPUCommandBuffer commandBuffer)
{
	vkCmdEndRenderPass(commandBuffer.backend);
}

void GfxRecordSetViewportCommand(GPUCommandBuffer commandBuffer, s64 width, s64 height)
{
	VkViewport viewport =
	{
		.x = 0.0f,
		.y = 0.0f,
		.width = (f32)width,
		.height = (f32)height,
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	vkCmdSetViewport(commandBuffer.backend, 0, 1, &viewport);
}

void GfxRecordSetScissorCommand(GPUCommandBuffer commandBuffer, u32 width, u32 height)
{
	VkRect2D scissor =
	{
		.offset = {0, 0},
		.extent = {width, height},
	};
	vkCmdSetScissor(commandBuffer.backend, 0, 1, &scissor);
}

void GfxRecordBindPipelineCommand(GPUCommandBuffer commandBuffer, GfxPipeline pipeline)
{
	vkCmdBindPipeline(commandBuffer.backend, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
}

#define VULKAN_VERTEX_BUFFER_BIND_ID 0

void GfxRecordBindVertexBufferCommand(GPUCommandBuffer commandBuffer, GfxBuffer vertexBuffer)
{
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(commandBuffer.backend, VULKAN_VERTEX_BUFFER_BIND_ID, 1, &vertexBuffer, &offset);
}

void GfxRecordBindIndexBufferCommand(GPUCommandBuffer commandBuffer, GfxBuffer indexBuffer)
{
	vkCmdBindIndexBuffer(commandBuffer.backend, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
}

void GfxDrawIndexedVertices(GPUCommandBuffer commandBuffer, s64 indexCount, s64 firstIndex, s64 vertexOffset)
{
	vkCmdDrawIndexed(commandBuffer.backend, indexCount, 1, firstIndex, vertexOffset, 0);
}

#if defined(__linux__)
	const char *GetRequiredVulkanSurfaceInstanceExtension()
	{
		return "VK_KHR_xlib_surface";
	}

	VkResult CreateVulkanSurface(PlatformWindow *window, VkInstance instance, VkSurfaceKHR *surface)
	{
		auto surfaceCreateInfo = VkXlibSurfaceCreateInfoKHR
		{
			.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
			.dpy = GetX11Display(),
			.window = window->x11Handle,
		};
		return vkCreateXlibSurfaceKHR(instance, &surfaceCreateInfo, NULL, surface);
	}
#endif

void InitializeGPU(PlatformWindow *win)
{
	PushContextAllocator(FrameAllocator("GPU"));
	Defer(PopContextAllocator());
	auto err = false;
	auto lib = OpenDLL("libvulkan.so", &err);
	if (error)
	{
		Abort("Could not open Vulkan DLL libvulkan.so.");
	}
#define VK_EXPORTED_FUNCTION(name) \
	name = (PFN_##name)GetDLLFunction(lib, #name, &err); \
	if (err) Abort("Failed to load Vulkan function %s: Vulkan version 1.1 required.", #name);
#define VK_GLOBAL_FUNCTION(name) \
	name = (PFN_##name)vkGetInstanceProcAddr(NULL, (const char *)#name); \
	if (!name) Abort("Failed to load Vulkan function %s: Vulkan version 1.1 required", #name);
#define VK_INSTANCE_FUNCTION(name)
#define VK_DEVICE_FUNCTION(name)
#include "VulkanFunction.h"
#undef VK_EXPORTED_FUNCTION
#undef VK_GLOBAL_FUNCTION
#undef VK_INSTANCE_FUNCTION
#undef VK_DEVICE_FUNCTION
	auto reqDevExts = NewStaticArray<String>(
		"VK_KHR_swapchain",
		"VK_EXT_descriptor_indexing",
		"VK_EXT_memory_budget");
	auto reqInstLayers = Array<String>{};
	if (DEBUG_BUILD)
	{
		reqInstLayers.Append("VK_LAYER_KHRONOS_validation");
	}
	auto reqInstExts = NewArray<String>(
		RequiredVulkanSurfaceInstanceExtension(),
		"VK_KHR_surface",
		"VK_KHR_get_physical_device_properties2");
	if (DEBUG_BUILD)
	{
		reqInstExts.Append("VK_EXT_debug_utils");
	}
	{
		auto v = u32{};
		vkEnumerateInstanceVersion(&v);
		if (VK_VERSION_MAJOR(v) < 1 || (VK_VERSION_MAJOR(v) == 1 && VK_VERSION_MINOR(v) < 1))
		{
			Abort("Vulkan version 1.1.0 or greater required: version %d.%d.%d is installed", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
		}
	}
	LogPrint(INFO_LOG, "GPU", "Using Vulkan version %d.%d.%d\n", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
	auto dbgInfo = VkDebugUtilsMessengerCreateInfoEXT
	{
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageType =
			VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
		.pfnUserCallback = VulkanDebugMessageCallback,
	};
	// Create instance.
	{
		auto numInstLayers = u32{};
		vkEnumerateInstanceLayerProperties(&numInstLayers, NULL);
		auto instLayers = NewArrayWithCount<VkLayerProperties>(numInstLayers);
		vkEnumerateInstanceLayerProperties(&numInstLayers, &instLayers[0]);
		LogPrint(INFO_LOG, "GPU", "Vulkan layers:\n");
		for (auto il : instLayers)
		{
			LogPrint(INFO_LOG, "GPU", "\t%s\n", il.layerName);
		}
		auto numInstExts = 0;
		VK_CHECK(vkEnumerateInstanceExtensionProperties(NULL, &numInstExts, NULL));
		auto instExts = NewArrayWithCount<VkExtensionProperties>(numInstExts);
		VK_CHECK(vkEnumerateInstanceExtensionProperties(NULL, &numInstExts, instExts));
		LogPrint(INFO_LOG, "GPU", "Available Vulkan instance extensions:\n");
		for (auto ie : instExts)
		{
			LogPrint(INFO_LOG, "\t%s\n", ie.extensionName);
		}
		auto ai = VkApplicationInfo
		{
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.pApplicationName = "Jaguar",
			.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
			.pEngineName = "Jaguar",
			.engineVersion = VK_MAKE_VERSION(1, 0, 0),
			.apiVersion = VK_MAKE_VERSION(1, 2, 0),
		};
		// @TODO: Check that all required extensions/layers are available.
		auto ci = VkInstanceCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pApplicationInfo = &ai,
			.enabledLayerCount = reqInstLayers.count,
			.ppEnabledLayerNames = &reqInstLayers[0],
			.enabledExtensionCount = reqInstExts.count,
			.ppEnabledExtensionNames = &reqInstExts[0],
		};
		if (DEBUG_BUILD)
		{
			ci.pNext = &dbgInfo;
		}
		VK_CHECK(vkCreateInstance(&ci, NULL, &vkInstance));
	}
#define VK_EXPORTED_FUNCTION(name)
#define VK_GLOBAL_FUNCTION(name)
#define VK_INSTANCE_FUNCTION(name) \
	name = (PFN_##name)vkGetInstanceProcAddr(vkInstance, (const char *)#name); \
	if (!name) Abort("Failed to load Vulkan function %s: Vulkan version 1.1 required", #name);
#define VK_DEVICE_FUNCTION(name)
#include "VulkanFunction.h"
#undef VK_EXPORTED_FUNCTION
#undef VK_GLOBAL_FUNCTION
#undef VK_INSTANCE_FUNCTION
#undef VK_DEVICE_FUNCTION
	VK_CHECK(vkCreateDebugUtilsMessengerEXT(vkInstance, &dbgInfo, NULL, &vkDebugMessenger));
	NewVulkanSurface(win, vkInstance, vkSurface);
	// Select physical device.
	// @TODO: Rank physical device and select the best one?
	{
		auto foundSuitablePhysDev = false;
		auto numPhysDevs = u32{};
		VK_CHECK(vkEnumeratePhysicalDevices(vkInstance, &numPhysDevs, NULL));
		if (numPhysDevs == 0)
		{
			Abort("Could not find any physical devices.");
		}
		auto physDevs = NewArrayWithCount<VkPhysicalDevice>(numPhysDevs);
		VK_CHECK(vkEnumeratePhysicalDevices(vkInstance, &numPhysDevs, &physDevs[0]));
		for (auto pd : physDevs)
		{
			auto df = VkPhysicalDeviceFeatures{};
			vkGetPhysicalDeviceFeatures(pd, &df);
			if (!df.samplerAnisotropy || !df.shaderSampledImageArrayDynamicIndexing)
			{
				continue;
			}
			auto numDevExts = u32{};
			VK_CHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &numDevExts, NULL));
			auto devExts = NewArrayWithCount<VkExtensionProperties>(numDevExts);
			VK_CHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &numDevExts, devExts));
			auto foundExt = false;
			for (auto rde : reqDevExts)
			{
				auto found = false;
				for (auto de : devExts)
				{
					if (de.extensionName == rde)
					{
						foundExt = true;
						goto outer;
					}
				}
			}
			outer:
			if (!foundExt)
			{
				continue;
			}
			// Make sure the swap chain is compatible with our window surface.
			// If we have at least one supported surface format and present mode, we will consider the device.
			// @TODO: Should we die if error, or just skip this physical device?
			auto numSurfFmts = u32{};
			VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vkSurface, &numSurfFmts, NULL));
			auto numPresentModes = u32{};
			VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, vkSurface, &numPresentModes, NULL));
			if (numSurfFmts == 0 || numPresentModes == 0)
			{
				continue;
			}
			// Select the best swap chain settings.
			auto surfFmts = NewArrayWithCount<VkSurfaceFormatKHR>(numSurfFmts);
			VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vkSurface, &numSurfFmts, &surfFmts[0]));
			auto surfFmt = surfFmts[0];
			if (numSurfFmts == 1 && surfFmts[0].format == VK_FORMAT_UNDEFINED)
			{
				// No preferred format, so we get to pick our own.
				surfFmt = VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
			}
			else
			{
				for (auto sf : surfFmts)
				{
					if (sf.format == VK_FORMAT_B8G8R8A8_UNORM && sf.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					{
						surfFmt = sf;
						break;
					}
				}
			}
			// VK_PRESENT_MODE_IMMEDIATE_KHR is for applications that don't care about tearing, or have some way of synchronizing their rendering with the display.
			// VK_PRESENT_MODE_MAILBOX_KHR may be useful for applications that generally render a new presentable image every refresh cycle, but are occasionally early.
			// In this case, the application wants the new image to be displayed instead of the previously-queued-for-presentation image that has not yet been displayed.
			// VK_PRESENT_MODE_FIFO_RELAXED_KHR is for applications that generally render a new presentable image every refresh cycle, but are occasionally late.
			// In this case (perhaps because of stuttering/latency concerns), the application wants the late image to be immediately displayed, even though that may mean some tearing.
			auto presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
			auto presentModes = NewArrayWithCount<VkPresentModeKHR>(numPresentModes);
			VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, vkSurface, &numPresentModes, &presentModes[0]));
			for (auto pm : presentModes)
			{
				if (pm == VK_PRESENT_MODE_MAILBOX_KHR)
				{
					presentMode = pm;
					break;
				}
			}
			auto numQueueFams = u32{};
			vkGetPhysicalDeviceQueueFamilyProperties(pd, &numQueueFams, NULL);
			auto queueFams = NewArrayWithCount<VkQueueFamilyProperties>(numQueueFams);
			vkGetPhysicalDeviceQueueFamilyProperties(pd, &numQueueFams, &queueFams[0]);
			auto graphicsQueueFam = -1;
			auto presentQueueFam = -1;
			auto computeQueueFam = -1;
			auto dedicatedTransferQueueFam = -1;
			for (auto i = 0; i < numQueueFams; i++)
			{
				if (queueFams[i].queueCount == 0)
				{
					continue;
				}
				if (queueFams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
				{
					graphicsQueueFam = i;
				}
				if (queueFams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
				{
					computeQueueFam = i;
				}
				if ((queueFams[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(queueFams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
				{
					dedicatedTransferQueueFam = i;
				}
				auto ps = u32{};
				VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, vkSurface, &ps));
				if (ps)
				{
					presentQueueFam = i;
				}
			}
			if (graphicsQueueFam != -1 && presentQueueFam != -1 && computeQueueFam != -1)
			{
				vkPhysicalDevice = availablePhysicalDevices[i];
				vkQueueFamilies[VulkanGraphicsQueue] = graphicsQueueFam;
				// The graphics queue family can always be used as the transfer queue family, but if
				// we can find a dedicated transfer queue family, use that instead.
				if (dedicatedTransferQueueFam != -1)
				{
					vkQueueFamilies[VulkanTransferQueue] = dedicatedTransferQueueFam;
				}
				else
				{
					vkQueueFamilies[VulkanTransferQueue] = graphicsQueueFam;
				}
				vkQueueFamilies[VulkanComputeQueue] = computeQueueFam;
				vkPresentQueueFamily = presentQueueFam;
				vkSurfaceFormat = surfFmt;
				vkPresentMode = presentMode;
				foundSuitablePhysDev = true;
				LogPrint(INFO_LOG, "GPU", "Vulkan device extensions:\n");
				for (auto de : devExts)
				{
					LogPrint(INFO_LOG, "\t%s\n", de.extensionName);
				}
				break;
			}
		}
		if (!foundSuitablePhysDev)
		{
			Abort("Could not find suitable physical device.\n");
		}
	}
	// Physical device info.
	{
		for (auto i = 0; i < VulkanMemoryTypeCount; i++)
		{
			auto e = (VulkanMemoryType)i;
			switch (e)
			{
			case VulkanGPUOnlyMemory:
			{
				vkMemoryTypeToMemoryFlags[i] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			} break;
			case VulkanCPUToGPUMemory:
			{
				vkMemoryTypeToMemoryFlags[i] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			} break;
			case VulkanGPUToCPUMemory:
			{
				vkMemoryTypeToMemoryFlags[i] =
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
					| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
					| VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
			} break;
			default:
			{
				Abort("Unknown memory type %d.", e);
			} break;
			}
		}
		auto devProps = VkPhysicalDeviceProperties{};
		vkGetPhysicalDeviceProperties(vkPhysicalDevice, &devProps);
		vkBufferImageGranularity = devProps.limits.bufferImageGranularity;
		auto memProps = VkPhysicalDeviceMemoryProperties{};
		vkGetPhysicalDeviceMemoryProperties(vkPhysicalDevice, &memProps);
		vkMemoryHeapCount = memProps.memoryHeapCount;
		for (auto i = 0; i < VulkanMemoryTypeCount; i++)
		{
			auto found = false;
			for (auto j = 0; j < memProps.memoryTypeCount; j++)
			{
				auto memFlags = vkMemoryTypeToMemoryFlags[i];
				if ((memProps.memoryTypes[j].propertyFlags & memFlags) == memFlags)
				{
					vkMemoryTypeToMemoryIndex[i] = j;
					vkMemoryTypeToHeapIndex[i] = memProps.memoryTypes[j].heapIndex;
					found = true;
					break;
				}
			}
			if (!found)
			{
				Abort("Unable to find GPU memory index and heap index for memory type %d.", e);
			}
		}
	}
	// Create logical device.
	{
		auto queuePrio = 1.0f;
		auto queueCIs = Array<VkDeviceQueueCreateInfo>{};
		queueCIs.Append(
			VkDeviceQueueCreateInfo
			{
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = vkGraphicsQueueFamily,
				.queueCount = 1,
				.pQueuePriorities = &queuePrio,
			});
		if (vkGraphicsQueueFamily != vkPresentQueueFamily)
		{
			queueCIs.Append(
				VkDeviceQueueCreateInfo
				{
					.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					.queueFamilyIndex = vkPresentQueueFamily,
					.queueCount = 1,
					.pQueuePriorities = &queuePrio,
				});
		}
		if (vkTransferQueueFamily != vkGraphicsQueueFamily)
		{
			queueCIs.Append(
				VkDeviceQueueCreateInfo
				{
					.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					.queueFamilyIndex = vkTransferQueueFamily,
					.queueCount = 1,
					.pQueuePriorities = &queuePrio,
				});
		}
		auto pdf = VkPhysicalDeviceFeatures
		{
			.samplerAnisotropy = VK_TRUE,
		};
		auto pddif = VkPhysicalDeviceDescriptorIndexingFeaturesEXT
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT,
			.runtimeDescriptorArray = VK_TRUE,
		};
		auto ci = VkDeviceCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext = &pddif,
			.queueCreateInfoCount = queueCIs.count,
			.pQueueCreateInfos = &queueCIs[0],
			.enabledLayerCount = reqInstLayers.count,
			.ppEnabledLayerNames = &reqInstLayers[0],
			.enabledExtensionCount = reqDevExts.count,
			.ppEnabledExtensionNames = &reqDevExts[0],
			.pEnabledFeatures = &pdf,
		};
		VK_CHECK(vkCreateDevice(vkPhysicalDevice, &ci, NULL, &vkDevice));
	}
#define VK_EXPORTED_FUNCTION(name)
#define VK_GLOBAL_FUNCTION(name)
#define VK_INSTANCE_FUNCTION(name)
#define VK_DEVICE_FUNCTION(name) \
	name = (PFN_##name)vkGetDeviceProcAddr(vkDevice, (const char *)#name); \
	if (!name) Abort("Failed to load Vulkan function %s: Vulkan version 1.1 is required.", #name);
#include "VulkanFunction.h"
#undef VK_EXPORTED_FUNCTION
#undef VK_GLOBAL_FUNCTION
#undef VK_INSTANCE_FUNCTION
#undef VK_DEVICE_FUNCTION
	LogVulkanInitialization();
	// Create command queues.
	for (auto i = 0; i < VulkanQueueTypeCount; i++)
	{
		auto e = (VulkanQueueType)i;
		switch (e)
		{
		case VulkanGraphicsQueue:
		{
			vkGetDeviceQueue(vkDevice, vkGraphicsQueueFamily, 0, &vkGraphicsQueue); // No return.
		} break;
		case VulkanTransferQueue:
		{
			vkGetDeviceQueue(vkDevice, vkTransferQueueFamily, 0, &vkTransferQueue); // No return.
		} break;
		case VulkanComputeQueue:
		{
			vkGetDeviceQueue(vkDevice, vkComputeQueueFamily, 0, &vkComputeQueue); // No return.
		} break;
		}
	}
	vkGetDeviceQueue(vkDevice, vkPresentQueueFamily, 0, &vkPresentQueue); // No return.
	// Create the presentation semaphores.
	{
		auto ci = VkSemaphoreCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};
		for (auto i = 0; i < GPU_MAX_FRAMES_IN_FLIGHT; i++)
		{
			if (vkQueueFamilies[GPUGraphicsQueue] != vkQueueFamilies[GPUGraphicsQueue])
			{
				CheckVkResult(vkCreateSemaphore(vkDevice, &ci, NULL, &vkImageOwnershipSemaphores[i]));
			}
		}
	}
	// Initialize memory allocators.
	{
		for (auto i = 0; i < VulkanMemoryTypeCount; i++)
		{
			auto e = (VulkanMemoryType)i;
			switch (e)
			{
			case VulkanGPUOnlyMemory:
			{
				vkGPUBlockAllocator.memoryType = VulkanGPUOnlyMemory;
			} break;
			case VulkanCPUToGPUMemory:
			{
				vkGPUBlockAllocator.memoryType = VulkanCPUToGPUMemory;
			} break;
			case VulkanGPUToCPUMemory:
			{
				vkGPUToCPUBlockAllocator.memoryType = VulkanGPUToCPUMemory;
			} break;
			default:
			{
				Abort("Unknown memory type %d.", e);
			} break;
			}
		}
		vkCPUToGPURingAllocator = NewVulkanMemoryRingAllocator(VulkanCPUToGPUMemory)
	}
	// Create command pools.
	{
		auto ci = VkCommandPoolCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			//.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		};
		for (auto i = 0; i < VulkanQueueTypeCount; i++)
		{
			ci.queueFamilyIndex = vkQueueFamilies[i];
			CheckVkResult(vkCreateCommandPool(vkDevice, &ci, NULL, &vkAsyncCommandPools[i]));
		}
		vkThreadLocal.Resize(WorkerThreadCount());
		for (auto &tl : vkThreadLocal)
		{
			for (auto i = 0; i < VulkanQueueCount; i++)
			{
				ci.queueFamilyIndex = vkQueueFamilies[i];
				for (auto j = 0; j < VulkanMaxFramesInFlight; j++)
				{
					CheckVkResult(vkCreateCommandPool(vkDevice, &ci, NULL, &tl.frameCommandPools[i][j]));
				}
			}
		}
	}
	// Create swapchain.
	{
		auto surfCaps = VkSurfaceCapabilitiesKHR{};
		CheckVkResult(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkPhysicalDevice, vkSurface, &surfCaps));
		auto ext = VkExtent2D{};
		if (surfCaps.currentExtent.width == U32_MAX && surfCaps.currentExtent.height == U32_MAX) // Indicates Vulkan will accept any extent dimension.
		{
			ext.width = RenderWidth();
			ext.height = RenderHeight();
		}
		else
		{
			ext.width = Maximum(surfCaps.minImageExtent.width, Minimum(surfCaps.maxImageExtent.width, RenderWidth()));
			ext.height = Maximum(surfCaps.minImageExtent.height, Minimum(surfCaps.maxImageExtent.height, RenderHeight()));
		}
		// @TODO: Why is this a problem?
		if (ext.width != RenderWidth() && ext.height != RenderHeight())
		{
			Abort("Swapchain image dimensions do not match the window dimensions: swapchain %ux%u, window %ux%u.", ext.width, ext.height, RenderWidth(), RenderHeight());
		}
		auto minImgs = surfCaps.minImageCount + 1;
		if (surfCaps.maxImageCount > 0 && (minImgs > surfCaps.maxImageCount))
		{
			minImgs = surfCaps.maxImageCount;
		}
		auto ci = VkSwapchainCreateInfoKHR
		{
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = vkSurface,
			.minImageCount = minImgs,
			.imageFormat = vkSurfaceFormat.format,
			.imageColorSpace  = vkSurfaceFormat.colorSpace,
			.imageExtent = ext,
			.imageArrayLayers = 1,
			.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			.preTransform = sc.currentTransform,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = vkPresentMode,
			.clipped = 1,
			.oldSwapchain = NULL,
		};
		if (vkGraphicsQueueFamily != vkPresentQueueFamily)
		{
			auto fams = NewStaticArray<u32>(
				vkGraphicsQueueFamily,
				vkPresentQueueFamily);
			ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			ci.queueFamilyIndexCount = fams.Count();
			ci.pQueueFamilyIndices = &fams[0];
		}
		else
		{
			ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}
		CheckVkResult(vkCreateSwapchainKHR(vkDevice, &ci, NULL, &vkSwapchain));
	}
	// Get swapchain images.
	{
		auto numImgs = u32{};
		CheckVkResult(vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &numImgs, NULL));
		vkSwapchainImages.Resize(numImgs);
		CheckVkResult(vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &numImgs, &vkSwapchainImages[0]));
		vkSwapchainImageViews.Resize(numImgs);
		for (auto i = 0; i < numImgs; i++)
		{
			auto cm = VkComponentMapping 
			{
				.r = VK_COMPONENT_SWIZZLE_IDENTITY,
				.g = VK_COMPONENT_SWIZZLE_IDENTITY,
				.b = VK_COMPONENT_SWIZZLE_IDENTITY,
				.a = VK_COMPONENT_SWIZZLE_IDENTITY,
			};
			auto isr = VkImageSubresourceRange 
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			};
			auto ci = VkImageViewCreateInfo
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = vkSwapchainImages[i],
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = vkSurfaceFormat.format,
				.components = cm,
				.subresourceRange = isr,
			};
			CheckVkResult(vkCreateImageView(vkDevice, &ci, NULL, &vkSwapchainImageViews[i]));
		}
	}
}

GPUCommandBuffer NewGPUFrameGraphicsCommandBuffer()
{
}

GPUCommandBuffer NewGPUFrameTransferCommandBuffer()
{
}

GPUCommandBuffer NewGPUFrameComputeCommandBuffer()
{
}

GPUCommandBuffer NewGPUAsyncGraphicsCommandBuffer()
{
}

GPUCommandBuffer NewGPUAsyncTransferCommandBuffer()
{
}

GPUCommandBuffer NewGPUAsyncComputeCommandBuffer()
{
}

void QueueVulkanFrameCommandBuffer(GPUCommandBuffer cb)
{
	vkFrameQueueLocks[cb.queueType].Lock();
	Defer(vkFrameQueueLocks.Unlock());
	vkFrameQueues[FrameIndex()].Append(cb);
}

void QueueVulkanAsyncCommandBuffer(GPUCommandBuffer cb, VulkanMapData *md, bool *signalOnCompletion)
{
	// @TODO: We could do this in a possibly faster way by keeping two thread-local async queues and
	// switching off between them.  This would be more complicated but it would avoid all lock
	// contention which might become high especially when we need to take the lock and do a
	// vkQueueSubmit. Would this even make a difference? Not sure.
	vkAsyncQueueLocks[cb.queueType].Lock();
	Defer(vkAsyncQueueLocks.Unlock());
	vkAsyncQueues[cb.queueType][].Append(
	{
		cb,
		signalOnCompletion,
	});
	vkAsyncSignals[cb.queueType][].Append(signalOnCompletion);
}

GPUBuffer NewGPUUniformBuffer(s64 size)
{
}

GPUBuffer NewGPUIndexBuffer(s64 size)
{
}

GPUBuffer NewGPUVertexBuffer(s64 size)
{
	auto ci = VkBufferCreateInfo
	{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		.size = size,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	auto buf = GPUBuffer{};
	CheckVkResult(vkCreateBuffer(vkDevice, &ci, NULL, &buf.vk));
	auto err = false;
	auto mem = vkGPUOnlyBlockAllocator.Allocate(size, &err);
	if (err)
	{
		Abort("GPU", "Failed to allocate vulkan buffer memory."); // @TODO: Try to handle this case?
	}
	CheckVkResult(vkBindBufferMemory(vkDevice, buf, mem, 0));
	vkBufferData.Insert(
		buf,
		VulkanBufferData
		{
			.usage = u,
			.memory = mem,
		});
	return buf;
}

void *GPUBuffer::MapFrame()
{
	auto dat = vkBufferData.LookupPointer(this->buffer);
	if (!dat)
	{
		Abort("GPU", "Failed to Map GPU buffer: vulkan buffer was not initialized.");
	}
	auto ci = VkBufferCreateInfo
	{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = dat->memory.size,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	auto uploadBuf = VkBuffer{};
	CheckVkResult(vkCreateBuffer(vkDevice, &ci, NULL, &uploadBuf));
	auto mem = vkCPUToGPURingAllocator.Allocate(dat->size);
	CheckVkResult(vkBindBufferMemory(vkDevice, uploadBuf, mem, 0));
	auto ptr = (void *){};
	CheckVkResult(vkMapMemory(vkDevice, mem, 0, dat->size, 0, &ptr));
	vkMapData.Insert(
		ptr,
		VulkanMapData
		{
			.buffer = uploadBuf,
			.memory = mem,
		});
	return ptr;
}

void *GPUBuffer::MapAsync()
{
	auto dat = vkBufferData.LookupPointer(this->buffer);
	if (!dat)
	{
		Abort("GPU", "Failed to Map GPU buffer: vulkan buffer was not initialized.");
	}
	auto ci = VkBufferCreateInfo
	{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = dat->size,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	auto uploadBuf = VkBuffer{};
	CheckVkResult(vkCreateBuffer(vkDevice, &ci, NULL, &uploadBuf));
	auto mem = vkCPUToGPUBlockAllocator.Allocate(dat->size);
	if (!mem)
	{
		Abort("GPU", "Failed to allocate transfer buffer memory."); // @TODO: Handle this case.
	}
	CheckVkResult(vkBindBufferMemory(vkDevice, uploadBuf, mem, 0));
	auto ptr = (void *){};
	CheckVkResult(vkMapMemory(vkDevice, mem, 0, dat->size, 0, &ptr));
	vkMapData.Insert(
		ptr,
		VulkanMapData
		{
			.buffer = uploadBuf,
			.memory = mem,
			.size = dat->size,
			#if DEBUG_BUILD
				.async = true,
			#endif
		});
	return ptr;
}

void FreeGPUFrameBuffers()
{
}

void FreeGPUFrameMemory()
{
}

void FreeGPUAsyncBuffers()
{
}

void FreeGPUAsyncMemory()
{
}

void GPUBuffer::FlushMapFrame(void **m)
{
	auto dat = vkMapData.LookupPointer(*m);
	Assert(!dat->async);
	if (!dat)
	{
		LogPrint("GPU", "Failed to flush buffer map: buffer needs to call MapFrameSync before FlushMapFrame.\n");
		return;
	}
	Defer(vkMapData.Remove(*m));
	auto cb = NewGPUTransferCommandBuffer(VulkanTransferQueue, dat->sync);
	cb.CopyBuffer(dat->buffer, this->buffer, dat->size, 0, 0);
	QueueVulkanFrameCommandBuffer(cb);
	*m = NULL;
	vkThreadLocal[ThreadIndex()].frameTransferBuffers.Append(dat->buffer);
	vkThreadLocal[ThreadIndex()].frameTransferMemory.Append(dat->memory);
}

void GPUBuffer::FlushMapAsync(void **m, bool *signalOnCompletion)
{
	auto dat = vkMapData.LookupPointer(*m);
	Assert(dat->async);
	if (!dat)
	{
		LogPrint("GPU", "Failed to flush buffer map: buffer needs to call MapAsync before FlushMapAsync.\n");
		return;
	}
	Defer(vkMapData.Remove(*m));
	auto cb = NewGPUAsyncTransferCommandBuffer();
	cb.CopyBuffer(dat->buffer, this->buffer, dat->size, 0, 0);
	QueueVulkanAsyncCommandBuffer(cb, dat, signalOnCompletion);
	*m = NULL;
	vkAsyncTransferResourceLock.Lock();
	Defer(vkAsyncTransferResourceLock.Unlock());
	vkAsyncTransferBuffers.Append(dat->buffer);
	vkAsyncTransferMemory.Append(dat->memory);
	vkAsyncTransferSignals.Append(signalOnCompletion);
}

void GPUBuffer::Free()
{
}

void StartGPUFrame()
{
	CheckVkResult(vkWaitForFences(vkDevice, 1, &vkFrameFences[vkFrameIndex], true, U32_MAX));
	CheckVkResult(vkResetFences(vkDevice, 1, &vkFrameFences[vkFrameIndex]));
	CheckVkResult(vkAcquireNextImageKHR(vkDevice, vkSwapchain, U64_MAX, vkImageAcquiredSemaphores[vkFrameIndex], NULL, &vkSwapchainImageIndex));
	ClearGPUMemoryForFrameIndex(renderGlobals.frameIndex);
	ClearGPUCommandPoolsForFrameIndex(renderGlobals.frameIndex);
}

void FinishGPUFrame()
{
}

GPUImage NewGPUImage()
{
}

GPUImageView NewGPUImageView()
{
}













#if 0
void TEMPORARY_VULKAN_SUBMIT(GfxCommandBuffer command_buffer, s32 current_frame_index, GfxFence fence)
{
	VkSemaphore wait_semaphores[] = {vulkanGlobals.imageAvailableSemaphores[current_frame_index]};
	VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	VkSemaphore signal_semaphores[] = {vulkanGlobals.renderFinishedSemaphores[current_frame_index]};
	VkSubmitInfo submit_info =
	{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = CArrayCount(wait_semaphores),
		.pWaitSemaphores = wait_semaphores,
		.pWaitDstStageMask = wait_stages,
		.commandBufferCount = 1,
		.pCommandBuffers = &command_buffer,
		.signalSemaphoreCount = CArrayCount(signal_semaphores),
		.pSignalSemaphores = signal_semaphores,
	};
	VK_CHECK(vkQueueSubmit(vulkanGlobals.graphicsQueue, 1, &submit_info, fence));
}


void update_vulkan_uniforms(M4 scene_projection, Camera *camera, Mesh_Instance *meshes, u32 *visible_meshes, u32 visible_mesh_count, u32 swapchain_image_index) {
	// @TODO: Does all of this need to happen every frame? Probably not!
	// Shadow map.
	{
		M4 model = m4_identity();
		M4 view = look_at((V3){2.0f, 2.0f, 2.0f}, (V3){0.0f, 0.0f, 0.0f}, (V3){0.0f, 0.0f, 1.0f});
		M4 projection = orthographic_projection(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 20.0f);
		shadow_map_ubo.world_to_clip_space = multiply_m4(multiply_m4(projection, view), model);
		stage_vulkan_data(&shadow_map_ubo, sizeof(Shadow_Map_UBO));
		transfer_staged_vulkan_data(vulkan_context.buffer_offsets.shadow_map.uniform);
	}

	M4 scene_projection_view = multiply_m4(scene_projection, camera->view_matrix);

	// Scene.
	{
		static const M4 shadow_map_clip_space_bias = {{
			{0.5, 0.0, 0.0, 0.5},
			{0.0, 0.5, 0.0, 0.5},
			{0.0, 0.0, 1.0, 0.0},
			{0.0, 0.0, 0.0, 1.0},
		}};
		M4 model = m4_identity();
		for (s32 i = 0; i < visible_mesh_count; i++) {
			set_matrix_translation(&model, meshes[visible_meshes[i]].transform.translation);
			dynamic_scene_ubo[i] = (Dynamic_Scene_UBO){
				.model_to_world_space           = multiply_m4(scene_projection_view, model),
				.world_to_clip_space            = multiply_m4(scene_projection_view, model),
				.world_to_shadow_map_clip_space = multiply_m4(multiply_m4(shadow_map_clip_space_bias, shadow_map_ubo.world_to_clip_space), model),
			};
		}
	}

	Scene_UBO scene_ubo = {
		.camera_position = camera->position,
	};
	stage_vulkan_data(&scene_ubo, sizeof(scene_ubo));
	transfer_staged_vulkan_data(vulkan_context.buffer_offsets.scene.uniform[swapchain_image_index]);

	//for (s32 i = 0; i < TEST_INSTANCES; i++) {
	for (u32 i = 0; i < visible_mesh_count; i++) {
		stage_vulkan_data(&dynamic_scene_ubo[i], sizeof(dynamic_scene_ubo[i]));
		transfer_staged_vulkan_data(vulkan_context.buffer_offsets.scene.dynamic_uniform[swapchain_image_index] + (i * vulkan_context.sizes.scene.aligned_dynamic_ubo));
		//transfer_staged_vulkan_data(xxx + vulkan_context.scene_dynamic_uniform_buffer_offsets[vulkan_context.current_swapchain_image_index] + (i * vulkan_context.aligned_scene_uniform_buffer_object_size));
		//u32 x = dubo_offset + i * align_to(sizeof(Dynamic_Scene_UBO), vulkan_context.minimum_uniform_buffer_offset_alignment);
		//assert(x % vulkan_context.minimum_uniform_buffer_offset_alignment == 0);
		//transfer_staged_vulkan_data(vulkan_context.scene_dynamic_uniform_buffer_offset + (i * vulkan_context.aligned_scene_dynamic_uniform_buffer_object_size));
	}

	// @TODO: Materials should be in a linear array.
	// @TODO: Load all materials from directory on startup.
	// @TODO: Only update materials when they need to be updated.
	for (u32 i = 0; i < visible_mesh_count; i++) {
		// WRONG!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! need a submesh index, not the mesh index
		Vulkan_Material material = {
			.albedo_map            = meshes[visible_meshes[i]].asset->materials[0].albedo_map,
			.normal_map            = meshes[visible_meshes[i]].asset->materials[0].normal_map,
			.metallic_map          = meshes[visible_meshes[i]].asset->materials[0].metallic_map,
			.roughness_map         = meshes[visible_meshes[i]].asset->materials[0].roughness_map,
			.ambient_occlusion_map = meshes[visible_meshes[i]].asset->materials[0].ambient_occlusion_map,
		};
		stage_vulkan_data(&material, sizeof(material));
		transfer_staged_vulkan_data(vulkan_context.buffer_offsets.scene.materials + (i * sizeof(Vulkan_Material)));
	}

	M4 m = m4_identity();
	set_matrix_translation(&m, (V3){-0.398581, 0.000000, 1.266762});
	Flat_Color_UBO ubo = {
		.color = {1.0, 0.0f, 0.0f, 1.0f},
		.model_view_projection = scene_projection_view,
	};
	stage_vulkan_data(&ubo, sizeof(ubo));
	transfer_staged_vulkan_data(vulkan_context.buffer_offsets.flat_color.uniform);
}

void build_vulkan_command_buffer(Mesh_Instance *meshes, u32 *visible_meshes, u32 visible_mesh_count, Render_Context *render_context, u32 swapchain_image_index) {
	VkCommandBuffer command_buffer = vulkan_context.command_buffers[swapchain_image_index];
	vkResetCommandBuffer(command_buffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);

	VkClearValue clear_color = {{{0.04f, 0.19f, 0.34f, 1.0f}}};
	VkClearValue clear_depth_stencil = {{{1.0f, 0.0f}}};
	VkClearValue clear_values[] = {
		clear_color,
		clear_depth_stencil,
	};
	VkBuffer vertex_buffers[] = {vulkan_context.gpu_buffer};
	VkDeviceSize offsets[] = {0};

	VkCommandBufferBeginInfo command_buffer_begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
	};
	VK_CHECK(vkBeginCommandBuffer(command_buffer, &command_buffer_begin_info));

	/*
	// Shadow map.
	{
		VkRenderPassBeginInfo render_pass_begin_info = {
			.sType                    = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass               = vulkan_context.render_passes.shadow_map,
			.framebuffer              = vulkan_context.xframebuffers.shadow_map,
			.renderArea.extent.width  = SHADOW_MAP_WIDTH,
			.renderArea.extent.height = SHADOW_MAP_HEIGHT,
			.clearValueCount          = 1,
			.pClearValues             = &clear_depth_stencil,
		};
		vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport viewport = {
			.x        = 0.0f,
			.y        = 0.0f,
			.width    = (f32)SHADOW_MAP_WIDTH,
			.height   = (f32)SHADOW_MAP_HEIGHT,
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};
		vkCmdSetViewport(command_buffer, 0, 1, &viewport);
		VkRect2D scissor = {
			.offset = {0, 0},
			.extent = {SHADOW_MAP_WIDTH, SHADOW_MAP_HEIGHT},
		};
		vkCmdSetScissor(command_buffer, 0, 1, &scissor);
		vkCmdSetDepthBias(command_buffer, SHADOW_MAP_CONSTANT_DEPTH_BIAS, 0.0f, SHADOW_MAP_SLOPE_DEPTH_BIAS); // Set depth bias, required to avoid shadow mapping artifacts.
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_context.pipelines.shadow_map_static);
		vkCmdBindVertexBuffers(command_buffer, VULKAN_VERTEX_BUFFER_BIND_ID, 1, &vulkan_context.gpu_buffer, &(VkDeviceSize){0});
		//vkCmdBindVertexBuffers(command_buffer, VULKAN_INSTANCE_BUFFER_BIND_ID, 1, &vulkan_context.gpu_buffer, &(VkDeviceSize){vulkan_context.buffer_offsets.instance_memory_segment});
		vkCmdBindIndexBuffer(command_buffer, vulkan_context.gpu_buffer, VULKAN_INDEX_MEMORY_SEGMENT_OFFSET, VK_INDEX_TYPE_UINT32);
		for (u32 i = 0; i < visible_mesh_count; i++) {
			u32 mesh_index = visible_meshes[i];
			u32 total_mesh_index_count = 0;
			for (u32 j = 0; j < submesh_counts[mesh_index]; j++) {
				vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_context.pipeline_layouts.shadow_map, SHADOW_MAP_UNIFORM_DESCRIPTOR_SET, 1, &vulkan_context.descriptor_sets.shadow_map.uniform, 0, NULL);
				vkCmdDrawIndexed(command_buffer, meshes[mesh_index].submesh_index_counts[j], 1, total_mesh_index_count + meshes[mesh_index].first_index, meshes[mesh_index].vertex_offset, 0);
				total_mesh_index_count += meshes[mesh_index].submesh_index_counts[j];
			}
		}
		vkCmdEndRenderPass(command_buffer);
	}
	*/

	// Scene.
	{
		VkRenderPassBeginInfo render_pass_begin_info = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = vulkan_context.render_pass,
			.framebuffer = vulkan_context.framebuffers[swapchain_image_index],
			.renderArea.offset = {0, 0},
			.renderArea.extent = vulkan_context.swapchain_image_extent,
			.clearValueCount = CArrayCount(clear_values),
			.pClearValues = clear_values,
		};
		vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport = {
			.x = 0.0f,
			.y = 0.0f,
			.width = (f32)vulkan_context.swapchain_image_extent.width,
			.height = (f32)vulkan_context.swapchain_image_extent.height,
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};
		vkCmdSetViewport(command_buffer, 0, 1, &viewport);

		VkRect2D scissor = {
			.offset = {0, 0},
			.extent = {vulkan_context.swapchain_image_extent.width, vulkan_context.swapchain_image_extent.height},
		};
		vkCmdSetScissor(command_buffer, 0, 1, &scissor);
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_context.pipelines.textured_static);
		// @TODO: Combine these vkCmdBindVertexBuffers calls.
		vkCmdBindVertexBuffers(command_buffer, VULKAN_INSTANCE_BUFFER_BIND_ID, 1, &vulkan_context.gpu_buffer, &(VkDeviceSize){vulkan_context.buffer_offsets.instance_memory_segment});
		// @TODO: Bind these all at the same time?
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_context.pipeline_layout, SCENE_UNIFORM_DESCRIPTOR_SET, 1, &vulkan_context.descriptor_sets.scene.uniform[swapchain_image_index], 0, NULL);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_context.pipeline_layout, SCENE_MATERIAL_DESCRIPTOR_SET, 1, &vulkan_context.descriptor_sets.scene.materials, 0, NULL);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_context.pipeline_layout, SCENE_SAMPLER_DESCRIPTOR_SET, 1, &vulkan_context.descriptor_sets.scene.sampler, 0, NULL);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_context.pipeline_layout, SCENE_TEXTURE_DESCRIPTOR_SET, 1, &vulkan_context.descriptor_sets.scene.texture, 0, NULL);
		for (u32 i = 0; i < visible_mesh_count; i++) {
			u32 mesh_index = visible_meshes[i];
			vkCmdBindVertexBuffers(command_buffer, VULKAN_VERTEX_BUFFER_BIND_ID, 1, &meshes[mesh_index].asset->gpu_mesh.memory->buffer, (u64 *)&meshes[mesh_index].asset->gpu_mesh.memory->offset);
			vkCmdBindIndexBuffer(command_buffer, meshes[mesh_index].asset->gpu_mesh.memory->buffer, meshes[mesh_index].asset->gpu_mesh.memory->offset + meshes[mesh_index].asset->gpu_mesh.indices_offset, VK_INDEX_TYPE_UINT32);
			u32 total_mesh_index_count = 0;
			for (u32 j = 0; j < meshes[mesh_index].asset->submesh_count; j++) {
				u32 offset_inside_dynamic_uniform_buffer = i * vulkan_context.sizes.scene.aligned_dynamic_ubo;
				vkCmdBindDescriptorSets(command_buffer,
				                        VK_PIPELINE_BIND_POINT_GRAPHICS,
				                        vulkan_context.pipeline_layout,
				                        SCENE_DYNAMIC_UNIFORM_DESCRIPTOR_SET,
				                        1,
				                        &vulkan_context.descriptor_sets.scene.dynamic_uniform[swapchain_image_index],
				                        1,
				                        &offset_inside_dynamic_uniform_buffer);
				//vkCmdDrawIndexed(command_buffer, meshes[mesh_index].submesh_index_counts[j], 1, total_mesh_index_count + meshes[mesh_index].first_index, meshes[mesh_index].vertex_offset, i);
				vkCmdDrawIndexed(command_buffer, meshes[mesh_index].asset->submesh_index_counts[j], 1, total_mesh_index_count, 0, i);
				total_mesh_index_count += meshes[mesh_index].asset->submesh_index_counts[j];
			}
		}

		for (u32 i = 0; i < render_context->debug_render_object_count; i++) {
			V4 push_constant = render_context->debug_render_objects[i].color;
			vkCmdPushConstants(command_buffer, vulkan_context.pipeline_layouts.flat_color, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(V4), (void *)&push_constant);
			vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_context.pipelines.flat_color);
			VkDeviceSize offsetss[] = {VULKAN_FRAME_VERTEX_MEMORY_SEGMENT_OFFSET + (vulkan_context.currentFrame * VULKAN_FRAME_VERTEX_MEMORY_SEGMENT_SIZE)};
			vkCmdBindVertexBuffers(command_buffer, 0, 1, &vulkan_context.staging_buffer, offsetss);
			vkCmdBindIndexBuffer(command_buffer, vulkan_context.staging_buffer, VULKAN_FRAME_INDEX_MEMORY_SEGMENT_OFFSET + (vulkan_context.currentFrame * VULKAN_FRAME_INDEX_MEMORY_SEGMENT_SIZE), VK_INDEX_TYPE_UINT32);
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan_context.pipeline_layouts.flat_color, FLAT_COLOR_UBO_DESCRIPTOR_SET_NUMBER, 1, &vulkan_context.descriptor_sets.flat_color.uniform, 0, NULL);
			vkCmdDrawIndexed(command_buffer, render_context->debug_render_objects[i].index_count, 1, render_context->debug_render_objects[i].first_index, render_context->debug_render_objects[i].vertex_offset, 0);
			//vkCmdDraw(command_buffer, render_context->debug_render_objects[i].vertex_count, 1, total_vertex_count, 0);
		}

		vkCmdEndRenderPass(command_buffer);
	}

	VK_CHECK(vkEndCommandBuffer(command_buffer));
}

// @TODO: A way to wait for the next frame without blocking? Or maybe we wait to see if there is any more work and then wait?
u32 GFX_Wait_For_Available_Frame(GFX_Context *context) {
	vulkan_context.currentFrame = vulkan_context.nextFrame;
	vulkan_context.nextFrame = (vulkan_context.nextFrame + 1) % GFX_MAX_FRAMES_IN_FLIGHT;
	vkWaitForFences(vulkan_context.device, 1, &vulkan_context.inFlightFences[vulkan_context.currentFrame], 1, UINT64_MAX);
	vkResetFences(vulkan_context.device, 1, &vulkan_context.inFlightFences[vulkan_context.currentFrame]);
	vkResetCommandPool(vulkan_context.device, context->thread_local[thread_index].command_pools[vulkan_context.currentFrame], 0);
	u32 swapchain_image_index = 0;
	VK_CHECK(vkAcquireNextImageKHR(vulkan_context.device, vulkan_context.swapchain, UINT64_MAX, vulkan_context.image_available_semaphores[vulkan_context.currentFrame], NULL, &swapchain_image_index));
	return swapchain_image_index;
}

void GFX_Transfer_Data(void *source, GFX_Memory_Location destination, u32 size) {
	//Platform_Lock_Mutex(&vulkan_context.mutex);
	stage_vulkan_data(source, size);
	transfer_staged_vulkan_data(destination.offset);
	//Platform_Unlock_Mutex(&vulkan_context.mutex);
	//transfer_staged_vulkan_data(vulkan_context.buffer_offsets.instance_memory_segment + offset_inside_instance_memory_segment);
}

GFX_Memory_Allocation *GFX_Acquire_Memory(GFX_Memory_Allocator *allocator, u32 size) {
	for (u32 i = 0; i < allocator->active_block->freed_allocation_count; i++) {
		Assert(0); // @TODO
	}
	GFX_Memory_Allocation *new_allocation = &allocator->active_block->active_allocations[allocator->active_block->active_allocation_count];
	*new_allocation = (GFX_Memory_Allocation){
		.block          = allocator->active_block,
		.size           = size,
		.mapped_pointer = allocator->active_block->mapped_pointer,
		.buffer         = allocator->active_block->buffer,
		.offset         = allocator->active_block->frontier,
	};
	allocator->active_block->active_allocation_count++;
	Assert(allocator->active_block->active_allocation_count < VULKAN_MAX_MEMORY_ALLOCATIONS_PER_BLOCK);
	allocator->active_block->frontier += size;
	Assert(allocator->active_block->frontier < VULKAN_MEMORY_BLOCK_SIZE);
	return new_allocation;
}

void vulkan_submit(Camera *camera, Mesh_Instance *meshes, u32 *visible_meshes, u32 visible_mesh_count, Render_Context *render_context, u32 swapchain_image_index) {
	LockMutex(&vulkan_context.mutex);

	//update_vulkan_uniforms(render_context->scene_projection, camera, meshes, visible_meshes, visible_mesh_count, swapchain_image_index);
	//vkWaitForFences(vulkan_context.device, 1, &the_fence, 1, UINT64_MAX);
	//build_vulkan_command_buffer(meshes, visible_meshes, visible_mesh_count, render_context, swapchain_image_index);

	VkSemaphore wait_semaphores[] = {vulkan_context.image_available_semaphores[vulkan_context.currentFrame]};
	VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	VkSemaphore signal_semaphores[] = {vulkan_context.render_finished_semaphores[vulkan_context.currentFrame]};

	VkSubmitInfo submit_info = {
		.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount   = CArrayCount(wait_semaphores),
		.pWaitSemaphores      = wait_semaphores,
		.pWaitDstStageMask    = wait_stages,
		.commandBufferCount   = 0,
		.pCommandBuffers      = &vulkan_context.command_buffers[swapchain_image_index],
		.signalSemaphoreCount = CArrayCount(signal_semaphores),
		.pSignalSemaphores    = signal_semaphores,
	};
	VK_CHECK(vkQueueSubmit(vulkan_context.graphics_queue, 1, &submit_info, vulkan_context.inFlightFences[vulkan_context.currentFrame]));

/* THIS STUFF NEEDS TO HAPPEN TO PROPERLY DIFFERENT GRAPHICS AND PRESENT QUEUES
    if (demo->separate_present_queue) {
        // If we are using separate queues, change image ownership to the
        // present queue before presenting, waiting for the draw complete
        // semaphore and signalling the ownership released semaphore when finished
        VkFence nullFence = VK_NULL_HANDLE;
        pipe_stage_flags = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &demo->draw_complete_semaphores[demo->frame_index];
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &demo->swapchain_image_resources[demo->current_buffer].graphics_to_present_cmd;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &demo->image_ownership_semaphores[demo->frame_index];
        err = vkQueueSubmit(demo->present_queue, 1, &submit_info, nullFence);
        assert(!err);
    }

    // If we are using separate queues we have to wait for image ownership,
    // otherwise wait for draw complete
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = NULL,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = (demo->separate_present_queue) ? &demo->image_ownership_semaphores[demo->frame_index]
                                                          : &demo->draw_complete_semaphores[demo->frame_index],
        .swapchainCount = 1,
        .pSwapchains = &demo->swapchain,
        .pImageIndices = &demo->current_buffer,
    };
*/

	VkSwapchainKHR swapchains[] = {vulkan_context.swapchain};
	VkPresentInfoKHR present_info = {
		.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = CArrayCount(signal_semaphores),
		.pWaitSemaphores    = signal_semaphores,
		.swapchainCount     = CArrayCount(swapchains),
		.pSwapchains        = swapchains,
		.pImageIndices      = &swapchain_image_index,
	};
	VK_CHECK(vkQueuePresentKHR(vulkan_context.present_queue, &present_info));

	vulkan_context.debug_vertex_memory_bytes_used = 0;
	vulkan_context.debug_index_memory_bytes_used = 0;
	vulkan_context.debug_vertex_count = 0;
	vulkan_context.debug_index_count = 0;

	UnlockMutex(&vulkan_context.mutex);
}

void Cleanup_Renderer() {
	vkDeviceWaitIdle(vulkan_context.device);

	if (debug) {
		vkDestroyDebugUtilsMessengerEXT(vulkan_context.instance, vulkan_context.debug_messenger, NULL);
	}

	for (s32 i = 0; i < SHADER_COUNT; i++) {
		for (s32 j = 0; j < vulkan_context.shaders[i].module_count; j++) {
			vkDestroyShaderModule(vulkan_context.device, vulkan_context.shaders[i].modules[j].module, NULL);
		}
	}

	for (s32 i = 0; i < GFX_MAX_FRAMES_IN_FLIGHT; i++) {
        vkWaitForFences(vulkan_context.device, 1, &vulkan_context.inFlightFences[i], VK_TRUE, UINT64_MAX);
		vkDestroyFence(vulkan_context.device, vulkan_context.inFlightFences[i], NULL);
		vkDestroySemaphore(vulkan_context.device, vulkan_context.image_available_semaphores[i], NULL);
		vkDestroySemaphore(vulkan_context.device, vulkan_context.render_finished_semaphores[i], NULL);
	}
	for (s32 i = 0; i < vulkan_context.num_swapchain_images; i++) {
		vkDestroyFramebuffer(vulkan_context.device, vulkan_context.framebuffers[i], NULL);
	}
	//vkDestroyPipeline(vulkan_context.device, vulkan_context.pipeline, NULL);
	vkDestroyRenderPass(vulkan_context.device, vulkan_context.render_pass, NULL);
	vkDestroyPipelineLayout(vulkan_context.device, vulkan_context.pipeline_layout, NULL);
	for (s32 i = 0; i < vulkan_context.num_swapchain_images; i++) {
		vkDestroyImageView(vulkan_context.device, vulkan_context.swapchain_image_views[i], NULL);
		//vkDestroyBuffer(vulkan_context.device, vulkan_context.uniform_buffers[i], NULL);
		//vkFreeMemory(vulkan_context.device, vulkan_context.uniform_buffers_memory[i], NULL);
	}
	vkDestroySwapchainKHR(vulkan_context.device, vulkan_context.swapchain, NULL);

	vkDestroySampler(vulkan_context.device, vulkan_context.textureSampler, NULL);
	//vkDestroyImageView(vulkan_context.device, vulkan_context.textureImageView, NULL);

	//vkDestroyImage(vulkan_context.device, vulkan_context.textureImage, NULL);
	vkFreeMemory(vulkan_context.device, vulkan_context.image_memory, NULL);

	vkDestroyDescriptorPool(vulkan_context.device, vulkan_context.descriptor_pool, NULL);
	//vkDestroyDescriptorSetLayout(vulkan_context.device, vulkan_context.descriptor_set_layout, NULL);

	//vkDestroyBuffer(vulkan_context.device, vulkan_context.vertex_buffer, NULL);
	//vkFreeMemory(vulkan_context.device, vulkan_context.vertex_buffer_memory, NULL);

	//vkDestroyBuffer(vulkan_context.device, vulkan_context.index_buffer, NULL);
	vkFreeMemory(vulkan_context.device, vulkan_context.gpu_memory, NULL);
	vkFreeMemory(vulkan_context.device, vulkan_context.shared_memory, NULL);

	vkDestroyCommandPool(vulkan_context.device, vulkan_context.command_pool, NULL);

	vkDeviceWaitIdle(vulkan_context.device);

	vkDestroyDevice(vulkan_context.device, NULL);
	vkDestroySurfaceKHR(vulkan_context.instance, vulkan_context.surface, NULL);

	Platform_Cleanup_Display();

	vkDestroyInstance(vulkan_context.instance, NULL); // On X11, the Vulkan instance must be destroyed after the display resources are destroyed.
}

#endif

#endif
