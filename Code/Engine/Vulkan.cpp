#ifdef VulkanBuild

#include "Vulkan.h"
#include "Render.h"
#include "Math.h"
#include "Basic/DLL.h"
#include "Basic/Array.h"
#include "Basic/Log.h"
#include "Basic/Memory.h"
#include "Common.h"

#define VK_EXPORTED_FUNCTION(name) PFN_##name name = NULL;
#define VK_GLOBAL_FUNCTION(name) PFN_##name name = NULL;
#define VK_INSTANCE_FUNCTION(name) PFN_##name name = NULL;
#define VK_DEVICE_FUNCTION(name) PFN_##name name = NULL;
#include "VulkanFunction.h"
#undef VK_EXPORTED_FUNCTION
#undef VK_GLOBAL_FUNCTION
#undef VK_INSTANCE_FUNCTION
#undef VK_DEVICE_FUNCTION

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

// @TODO: Make sure that all failable Vulkan calls are VkCheck'd.
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

enum VulkanMemoryType
{
	VulkanGPUOnlyMemory,
	VulkanCPUToGPUMemory,
	VulkanGPUToCPUMemory,
	VulkanMemoryTypeCount
};

struct VulkanMemoryFrameAllocator
{
	Spinlock lock;
	s64 capacity;
	s64 size;
	StaticArray<s64, VulkanMaxFramesInFlight> frameSizes;
	s64 start, end;
	StaticArray<Array<VulkanMemoryAllocation>, VulkanMaxFramesInFlight> allocations;
	VkDeviceMemory vkMemory;
	VulkanMemoryType memoryType;
	void *map;

	VulkanMemoryAllocation *Allocate(s64 size, s64 alignment, s64 frameIndex);
	void Free(s64 frameIndex);
};

struct VulkanMemoryBlock
{
	VkDeviceMemory vkMemory;
	void *map;
	s64 frontier;
	Array<VulkanMemoryAllocation> allocations;
};

struct VulkanMemoryBlockAllocator
{
	Spinlock lock;
	s64 blockSize;
	Array<VulkanMemoryBlock> blocks;
	VulkanMemoryType memoryType;

	VulkanMemoryAllocation *Allocate(s64 size, s64 align);
	void Deallocate(VulkanMemoryAllocation *a);
};

struct VulkanAsyncStagingBufferResources
{
	VulkanMemoryAllocation *memory;
	VkBuffer vkBuffer;
};

struct VulkanThreadLocal
{
	ValuePool<VkFence> fencePool;
	ValuePool<VkBuffer> imagePool;
	// Frame resources.
	StaticArray<StaticArray<FramePool<VkBuffer>, GPUBufferTypeCount>, VulkanMaxFramesInFlight> frameBufferPool;
	StaticArray<StaticArray<VkCommandPool, GPUQueueTypeCount>, VulkanMaxFramesInFlight> frameCommandPools;
	StaticArray<StaticArray<FramePool<VkCommandBuffer>, GPUQueueTypeCount>, VulkanMaxFramesInFlight> frameCommandBufferPool;
	StaticArray<StaticArray<Array<VkCommandBuffer>, GPUQueueTypeCount>, VulkanMaxFramesInFlight> frameQueuedCommandBuffers;
	// Async resources.
	// @TODO: Could use double buffers to avoid the locking.
	Spinlock asyncBufferLock;
	StaticArray<ValuePool<VkBuffer>, GPUBufferTypeCount> asyncBufferPool;
	Spinlock asyncCommandBufferLock;
	StaticArray<VkCommandPool, GPUQueueTypeCount> asyncCommandPools;
	StaticArray<ValuePool<VkCommandBuffer>, GPUQueueTypeCount> asyncCommandBufferPool;
	StaticArray<Array<VkCommandBuffer>, GPUQueueTypeCount> asyncQueuedCommandBuffers;
	StaticArray<Array<Array<VkCommandBuffer>>, GPUQueueTypeCount> asyncPendingCommandBuffers;
	StaticArray<Array<bool *>, GPUQueueTypeCount> asyncSignals;
	StaticArray<Array<GPUFence>, GPUQueueTypeCount> asyncFences;
	Spinlock asyncStagingBufferLock;
	Array<bool> asyncStagingSignals;
	Array<VulkanAsyncStagingBufferResources> asyncPendingStagingBuffers;
};

auto vkThreadLocal = Array<VulkanThreadLocal>{};
auto vkDebugMessenger = VkDebugUtilsMessengerEXT{};
auto vkInstance = VkInstance{};
auto vkPhysicalDevice = VkPhysicalDevice{};
auto vkDevice = VkDevice{};
auto vkQueueFamilies = StaticArray<u32, GPUQueueTypeCount>{};
auto vkQueues = StaticArray<VkQueue, GPUQueueTypeCount>{};
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
auto vkMemoryTypeToMemoryIndex = StaticArray<u32, VulkanMemoryTypeCount>{};
auto vkMemoryTypeToHeapIndex = StaticArray<u32, VulkanMemoryTypeCount>{};
auto vkGPUBlockAllocator = VulkanMemoryBlockAllocator{};
auto vkCPUToGPUBlockAllocator = VulkanMemoryBlockAllocator{};
auto vkGPUToCPUBlockAllocator = VulkanMemoryBlockAllocator{};
auto vkCPUToGPUFrameAllocator = VulkanMemoryFrameAllocator{};
// @TODO: Explain these and how they interact with the command queues...
// @TODO: We could store async queues in thread local storage and avoid locking.
//auto vkAsyncQueueLocks = StaticArray<SpinLock, GPUQueueTypeCount>{};
//auto vkAsyncQueues = StaticArray<Array<VkCommandBuffer>, GPUQueueTypeCount>{};
//auto vkAsyncSignals StaticArray<Array<bool *>, GPUQueueTypeCount>{};
//auto vkAsyncQueueIndices = StaticArray<(volatile s64), GPUQueueTypeCount>{}
//auto vkAsyncCommandPools = StaticArray<VkCommandPool, GPUQueueTypeCount>{}
// @TODO: Explain how command buffer queues work.
// @TODO: We could store frame queues in thread local storage and avoid locking.
auto vkFrameQueueLocks = StaticArray<Spinlock, GPUQueueTypeCount>{};
auto vkFrameQueues = StaticArray<Array<VkCommandBuffer>, GPUQueueTypeCount>{};
auto vkSwapchain = VkSwapchainKHR{};
auto vkSwapchainImageIndex = u32{}; // @TODO
auto vkSwapchainImages = Array<VkImage>{};
auto vkSwapchainImageViews = Array<VkImageView>{};
auto vkPipelineLayout = VkPipelineLayout{};
auto vkDescriptorPool = VkDescriptorPool{};
auto vkDescriptorSetLayouts = Array<VkDescriptorSetLayout>{}; 
auto vkDescriptorSets = Array<Array<VkDescriptorSet>>{};
auto vkDescriptorSetBuffers = Array<Array<VkBuffer>>{};
auto vkDescriptorSetUpdateFence = VkFence{};
// @TODO: Explain how semaphores work and interact.
auto vkImageOwnershipSemaphores = StaticArray<VkSemaphore, VulkanMaxFramesInFlight>{};
auto vkImageAcquiredSemaphores = StaticArray<VkSemaphore, VulkanMaxFramesInFlight>{};
auto vkDrawCompleteSemaphores = StaticArray<VkSemaphore, VulkanMaxFramesInFlight>{};
// @TODO: Could be thread-local lock-free double buffer.
auto vkFrameIndex = u32{};
auto vkFrameFences = StaticArray<VkFence, VulkanMaxFramesInFlight>{};

#define VkCheck(x)\
	do {\
		VkResult _r = (x);\
		if (_r != VK_SUCCESS) Abort("Vulkan", "VkCheck failed on '%s': %k\n", #x, VkResultToString(_r));\
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
		case (VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT):
			return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
		case (VK_ERROR_INCOMPATIBLE_VERSION_KHR):
			return "VK_ERROR_INCOMPATIBLE_VERSION_KHR";
		case (VK_ERROR_UNKNOWN):
			return "VK_ERROR_UNKNOWN";
		case (VK_THREAD_IDLE_KHR):
			return "VK_THREAD_IDLE_KHR";
		case (VK_THREAD_DONE_KHR):
			return "VK_THREAD_DONE_KHR";
		case (VK_OPERATION_DEFERRED_KHR):
			return "VK_OPERATION_DEFERRED_KHR";
		case (VK_OPERATION_NOT_DEFERRED_KHR):
			return "VK_OPERATION_NOT_DEFERRED_KHR";
		case (VK_ERROR_PIPELINE_COMPILE_REQUIRED_EXT):
			return "VK_ERROR_PIPELINE_COMPILE_REQUIRED_EXT";
	}
	return "UnknownVkResultCode";
}

u32 VulkanDebugMessageCallback(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *userData)
{
	auto log = LogLevel{};
	auto sevStr = String{};
	switch (sev)
	{
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
	{
		log = VerboseLog;
		sevStr = "Verbose";
	} break;
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
	{
		log = InfoLog;
		sevStr = "Info";
	} break;
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
	{
		log = ErrorLog;
		sevStr = "Warning";
	} break;
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
	{
		log = ErrorLog;
		sevStr = "Error";
	} break;
	case VK_DEBUG_UTILS_MESSAGE_SEVERITY_FLAG_BITS_MAX_ENUM_EXT:
	default:
	{
		log = ErrorLog;
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
	LogPrint(log, "Vulkan", "Vulkan: %k: %k: %s\n", sevStr, typeStr, data->pMessage);
    return 0;
}

void VulkanMemoryAllocation::Free()
{
	// @TODO
}

bool IsVulkanMemoryTypeCPUVisible(VulkanMemoryType t)
{
	if (t == VulkanCPUToGPUMemory || t == VulkanGPUToCPUMemory)
	{
		return true;
	}
	return false;
}

VulkanMemoryFrameAllocator NewVulkanMemoryFrameAllocator(s64 cap)
{
	auto ai = VkMemoryAllocateInfo
	{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = (VkDeviceSize)cap,
		.memoryTypeIndex = vkMemoryTypeToMemoryIndex[VulkanCPUToGPUMemory],
	};
	auto mem = VkDeviceMemory{};
	auto rc = vkAllocateMemory(vkDevice, &ai, NULL, &mem);
	if (rc == VK_ERROR_OUT_OF_DEVICE_MEMORY || rc == VK_ERROR_OUT_OF_HOST_MEMORY)
	{
		Abort("Vulkan", "Failed to allocate memory for frame allocator.");
	}
	VkCheck(rc);
	auto a = VulkanMemoryFrameAllocator
	{
		.capacity = cap,
		.vkMemory = mem,
	};
	for (auto i = 0; i < VulkanMaxFramesInFlight; i += 1)
	{
		a.allocations[i].SetAllocator(GlobalAllocator());
	}
	VkCheck(vkMapMemory(vkDevice, a.vkMemory, 0, cap, 0, &a.map));
	return a;
}

VulkanMemoryAllocation *VulkanMemoryFrameAllocator::Allocate(s64 size, s64 align, s64 frameIndex)
{
	this->lock.Lock();
	Defer(this->lock.Unlock());
	auto alignOffset = AlignAddress(this->end, align) - this->end;
	this->frameSizes[frameIndex] += alignOffset + size;
	this->size += alignOffset + size;
	if (this->size > this->capacity)
	{
		// @TODO: Fallback to block allocators.
		Abort("Vulkan", "Frame allocator ran out of space...");
	}
	this->allocations[frameIndex].Resize(this->allocations[frameIndex].count + 1);
	auto a = &this->allocations[frameIndex][this->allocations[frameIndex].count - 1];
	a->size = size;
	a->vkMemory = this->vkMemory;
	a->offset = (this->end + alignOffset) % this->capacity;
	a->map = (u8 *)this->map + a->offset;
	this->end = (this->end + alignOffset + size) % this->capacity;
	return a;
}

void VulkanMemoryFrameAllocator::Free(s64 frameIndex)
{
	this->start = (this->start + this->frameSizes[frameIndex]) % this->capacity;
	this->size -= this->frameSizes[frameIndex];
	this->frameSizes[frameIndex] = 0;
	this->allocations[frameIndex].Resize(0);
}

VulkanMemoryBlockAllocator NewVulkanMemoryBlockAllocator(VulkanMemoryType t, s64 blockSize)
{
	return
	{
		.blockSize = blockSize,
		.blocks = NewArrayIn<VulkanMemoryBlock>(GlobalAllocator(), 0),
		.memoryType = t,
	};
}

VulkanMemoryAllocation *VulkanMemoryBlockAllocator::Allocate(s64 size, s64 align)
{
	this->lock.Lock();
	Defer(this->lock.Unlock());
	Assert(size < this->blockSize);
	// @TODO: Try to allocate out of the freed allocations.
	if (this->blocks.count == 0 || AlignAddress(this->blocks[this->blocks.count - 1].frontier, align) + size > this->blockSize)
	{
		// Need to allocate a new block.
		this->blocks.Resize(this->blocks.count + 1);
		auto newBlock = &this->blocks[this->blocks.count - 1];
		auto ai = VkMemoryAllocateInfo
		{
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = (VkDeviceSize)this->blockSize,
			.memoryTypeIndex = vkMemoryTypeToMemoryIndex[this->memoryType],
		};
		if (!vkAllocateMemory(vkDevice, &ai, NULL, &newBlock->vkMemory))
		{
			Abort("Vulkan", "Failed to allocate GPU memory for block allocator..."); // @TODO
			return NULL;
		}
		newBlock->frontier = 0;
		newBlock->allocations = NewArrayIn<VulkanMemoryAllocation>(GlobalAllocator(), 0);
		if (IsVulkanMemoryTypeCPUVisible(this->memoryType))
		{
			VkCheck(vkMapMemory(vkDevice, newBlock->vkMemory, 0, this->blockSize, 0, &newBlock->map));
		}
		else
		{
			newBlock->map = NULL;
		}
	}
	auto b = &this->blocks[this->blocks.count - 1];
	b->allocations.Resize(b->allocations.count + 1);
	auto a = &b->allocations[b->allocations.count - 1];
	a->size = size;
	a->vkMemory = b->vkMemory;
	b->frontier = AlignAddress(b->frontier, align);
	a->offset = b->frontier;
	b->frontier += size;
	if (IsVulkanMemoryTypeCPUVisible(this->memoryType))
	{
		a->map = ((u8 *)b->map) + a->offset;
	}
	else
	{
		a->map = NULL;
	}
	Assert(b->frontier <= this->blockSize);
	return a;
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
	auto heaps = NewArray<GPUMemoryHeapInfo>(vkMemoryHeapCount);
	for (auto i = 0; i < vkMemoryHeapCount; i++)
	{
		heaps[i].usage = bp.heapUsage[i];
		heaps[i].budget = bp.heapBudget[i];
	}
	return heaps;
}

void PrintGPUMemoryHeapInfo()
{
	LogInfo("Vulkan", "GPU memory info:\n");
	auto hi = GPUMemoryHeapInfos();
	for (auto i = 0; i < hi.count; i++)
	{
		LogInfo(
			"Vulkan",
			"	Heap: %d, Usage: %fmb, Total: %fmb\n",
			i,
			BytesToMegabytes(hi[i].usage),
			BytesToMegabytes(hi[i].budget));
	}
}

#ifdef __linux__
	const char *RequiredVulkanSurfaceInstanceExtension()
	{
		return "VK_KHR_xcb_surface";
	}

	xcb_connection_t *XCBConnection();

	VkResult NewVulkanSurface(Window *w, VkInstance inst, VkSurfaceKHR *s)
	{
		auto ci = VkXcbSurfaceCreateInfoKHR
		{
			.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
			.connection = XCBConnection(),
			.window = w->xcbWindow,
		};
		return vkCreateXcbSurfaceKHR(inst, &ci, NULL, s);
	}
#endif

void InitializeGPU(Window *win)
{
	// @TODO:
	//PushContextAllocator(FrameAllocator("Vulkan"));
	//Defer(PopContextAllocator());
	auto err = false;
	auto lib = OpenDLL("libvulkan.so", &err);
	if (err)
	{
		Abort("Vulkan", "Could not open Vulkan DLL libvulkan.so.");
	}
#define VK_EXPORTED_FUNCTION(name) \
	name = (PFN_##name)lib.Lookup(#name, &err); \
	if (err) Abort("Vulkan", "Failed to load Vulkan function %s: Vulkan version 1.1 required.", #name);
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
	auto reqDevExts = MakeStaticArray<const char *>(
		"VK_KHR_swapchain",
		"VK_EXT_descriptor_indexing",
		"VK_EXT_memory_budget");
	auto reqInstLayers = Array<const char *>{};
	if (DebugBuild)
	{
		reqInstLayers.Append("VK_LAYER_KHRONOS_validation");
	}
	auto reqInstExts = MakeArray<const char *>(
		RequiredVulkanSurfaceInstanceExtension(),
		"VK_KHR_surface",
		"VK_KHR_get_physical_device_properties2");
	if (DebugBuild)
	{
		reqInstExts.Append("VK_EXT_debug_utils");
	}
	{
		auto version = u32{};
		vkEnumerateInstanceVersion(&version);
		if (VK_VERSION_MAJOR(version) < 1 || (VK_VERSION_MAJOR(version) == 1 && VK_VERSION_MINOR(version) < 1))
		{
			Abort("Vulkan", "Vulkan version 1.1.0 or greater required: version %d.%d.%d is installed", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
		}
		LogInfo("Vulkan", "Using Vulkan version %d.%d.%d\n", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
	}
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
		auto instLayers = NewArray<VkLayerProperties>(numInstLayers);
		vkEnumerateInstanceLayerProperties(&numInstLayers, &instLayers[0]);
		LogInfo("Vulkan", "Vulkan layers:\n");
		for (auto il : instLayers)
		{
			LogInfo("Vulkan", "\t%s\n", il.layerName);
		}
		auto numInstExts = u32{0};
		VkCheck(vkEnumerateInstanceExtensionProperties(NULL, &numInstExts, NULL));
		auto instExts = NewArray<VkExtensionProperties>(numInstExts);
		VkCheck(vkEnumerateInstanceExtensionProperties(NULL, &numInstExts, instExts.elements));
		LogInfo("Vulkan", "Available Vulkan instance extensions:\n");
		for (auto ie : instExts)
		{
			LogInfo("Vulkan", "\t%s\n", ie.extensionName);
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
			.enabledLayerCount = (u32)reqInstLayers.count,
			.ppEnabledLayerNames = reqInstLayers.elements,
			.enabledExtensionCount = (u32)reqInstExts.count,
			.ppEnabledExtensionNames = reqInstExts.elements,
		};
		if (DebugBuild)
		{
			ci.pNext = &dbgInfo;
		}
		VkCheck(vkCreateInstance(&ci, NULL, &vkInstance));
	}
#define VK_EXPORTED_FUNCTION(name)
#define VK_GLOBAL_FUNCTION(name)
#define VK_INSTANCE_FUNCTION(name) \
	name = (PFN_##name)vkGetInstanceProcAddr(vkInstance, (const char *)#name); \
	if (!name) Abort("Vulkan", "Failed to load Vulkan function %s: Vulkan version 1.1 required", #name);
#define VK_DEVICE_FUNCTION(name)
	#include "VulkanFunction.h"
#undef VK_EXPORTED_FUNCTION
#undef VK_GLOBAL_FUNCTION
#undef VK_INSTANCE_FUNCTION
#undef VK_DEVICE_FUNCTION
	VkCheck(vkCreateDebugUtilsMessengerEXT(vkInstance, &dbgInfo, NULL, &vkDebugMessenger));
	VkCheck(NewVulkanSurface(win, vkInstance, &vkSurface));
	// Select physical device.
	// @TODO: Rank physical device and select the best one?
	{
		auto foundSuitablePhysDev = false;
		auto numPhysDevs = u32{};
		VkCheck(vkEnumeratePhysicalDevices(vkInstance, &numPhysDevs, NULL));
		if (numPhysDevs == 0)
		{
			Abort("Vulkan", "Could not find any physical devices.");
		}
		auto physDevs = NewArray<VkPhysicalDevice>(numPhysDevs);
		VkCheck(vkEnumeratePhysicalDevices(vkInstance, &numPhysDevs, &physDevs[0]));
		for (auto pd : physDevs)
		{
			auto df = VkPhysicalDeviceFeatures{};
			vkGetPhysicalDeviceFeatures(pd, &df);
			if (!df.samplerAnisotropy || !df.shaderSampledImageArrayDynamicIndexing)
			{
				continue;
			}
			auto numDevExts = u32{};
			VkCheck(vkEnumerateDeviceExtensionProperties(pd, NULL, &numDevExts, NULL));
			auto devExts = NewArray<VkExtensionProperties>(numDevExts);
			VkCheck(vkEnumerateDeviceExtensionProperties(pd, NULL, &numDevExts, devExts.elements));
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
			VkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vkSurface, &numSurfFmts, NULL));
			auto numPresentModes = u32{};
			VkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, vkSurface, &numPresentModes, NULL));
			if (numSurfFmts == 0 || numPresentModes == 0)
			{
				continue;
			}
			// Select the best swap chain settings.
			auto surfFmts = NewArray<VkSurfaceFormatKHR>(numSurfFmts);
			VkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vkSurface, &numSurfFmts, &surfFmts[0]));
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
			auto presentModes = NewArray<VkPresentModeKHR>(numPresentModes);
			VkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, vkSurface, &numPresentModes, &presentModes[0]));
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
			auto queueFams = NewArray<VkQueueFamilyProperties>(numQueueFams);
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
				VkCheck(vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, vkSurface, &ps));
				if (ps)
				{
					presentQueueFam = i;
				}
			}
			if (graphicsQueueFam != -1 && presentQueueFam != -1 && computeQueueFam != -1)
			{
				vkPhysicalDevice = pd;
				vkQueueFamilies[GPUGraphicsQueue] = graphicsQueueFam;
				// The graphics queue family can always be used as the transfer queue family, but if
				// we can find a dedicated transfer queue family, use that instead.
				if (dedicatedTransferQueueFam != -1)
				{
					vkQueueFamilies[GPUTransferQueue] = dedicatedTransferQueueFam;
				}
				else
				{
					vkQueueFamilies[GPUTransferQueue] = graphicsQueueFam;
				}
				vkQueueFamilies[GPUComputeQueue] = computeQueueFam;
				vkPresentQueueFamily = presentQueueFam;
				vkSurfaceFormat = surfFmt;
				vkPresentMode = presentMode;
				foundSuitablePhysDev = true;
				LogInfo("Vulkan", "Vulkan device extensions:\n");
				for (auto de : devExts)
				{
					LogInfo("Vulkan", "\t%s\n", de.extensionName);
				}
				break;
			}
		}
		if (!foundSuitablePhysDev)
		{
			Abort("Vulkan", "Could not find suitable physical device.\n");
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
			case VulkanMemoryTypeCount:
			default:
			{
				Abort("Vulkan", "Unknown memory type %d.", e);
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
				Abort("Vulkan", "Unable to find GPU memory index and heap index for memory type %d.", i);
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
				.queueFamilyIndex = vkQueueFamilies[GPUGraphicsQueue],
				.queueCount = 1,
				.pQueuePriorities = &queuePrio,
			});
		if (vkQueueFamilies[GPUGraphicsQueue] != vkPresentQueueFamily)
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
		if (vkQueueFamilies[GPUTransferQueue] != vkQueueFamilies[GPUGraphicsQueue])
		{
			queueCIs.Append(
				VkDeviceQueueCreateInfo
				{
					.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					.queueFamilyIndex = vkQueueFamilies[GPUTransferQueue],
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
			.queueCreateInfoCount = (u32)queueCIs.count,
			.pQueueCreateInfos = queueCIs.elements,
			.enabledLayerCount = (u32)reqInstLayers.count,
			.ppEnabledLayerNames = reqInstLayers.elements,
			.enabledExtensionCount = (u32)reqDevExts.Count(),
			.ppEnabledExtensionNames = reqDevExts.elements,
			.pEnabledFeatures = &pdf,
		};
		VkCheck(vkCreateDevice(vkPhysicalDevice, &ci, NULL, &vkDevice));
	}
#define VK_EXPORTED_FUNCTION(name)
#define VK_GLOBAL_FUNCTION(name)
#define VK_INSTANCE_FUNCTION(name)
#define VK_DEVICE_FUNCTION(name) \
	name = (PFN_##name)vkGetDeviceProcAddr(vkDevice, (const char *)#name); \
	if (!name) Abort("Vulkan", "Failed to load Vulkan function %s: Vulkan version 1.1 is required.", #name);
#include "VulkanFunction.h"
#undef VK_EXPORTED_FUNCTION
#undef VK_GLOBAL_FUNCTION
#undef VK_INSTANCE_FUNCTION
#undef VK_DEVICE_FUNCTION
	//LogVulkanInitialization();
	// Create command queues.
	for (auto i = 0; i < GPUQueueTypeCount; i += 1)
	{
		vkGetDeviceQueue(vkDevice, vkQueueFamilies[i], 0, &vkQueues[i]); // No return.
	}
	vkGetDeviceQueue(vkDevice, vkPresentQueueFamily, 0, &vkPresentQueue); // No return.
	// Create the presentation semaphores.
	{
		auto ci = VkSemaphoreCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};
		for (auto i = 0; i < VulkanMaxFramesInFlight; i++)
		{
			if (vkPresentQueueFamily != vkQueueFamilies[GPUGraphicsQueue])
			{
				VkCheck(vkCreateSemaphore(vkDevice, &ci, NULL, &vkImageOwnershipSemaphores[i]));
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
			case VulkanMemoryTypeCount:
			default:
			{
				Abort("Vulkan", "Unknown memory type %d.", e);
			} break;
			}
		}
		vkCPUToGPUFrameAllocator = NewVulkanMemoryFrameAllocator(MegabytesToBytes(256));
	}
	vkThreadLocal.Resize(WorkerThreadCount());
	// Create command pools.
	{
		auto ci = VkCommandPoolCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			//.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		};
		for (auto &tl : vkThreadLocal)
		{
			for (auto i = 0; i < GPUQueueTypeCount; i++)
			{
				ci.queueFamilyIndex = vkQueueFamilies[i];
				VkCheck(vkCreateCommandPool(vkDevice, &ci, NULL, &tl.asyncCommandPools[i]));
			}
		}
		for (auto &tl : vkThreadLocal)
		{
			for (auto i = 0; i < VulkanMaxFramesInFlight; i += 1)
			{
				for (auto j = 0; j < GPUQueueTypeCount; j += 1)
				{
					ci.queueFamilyIndex = vkQueueFamilies[j];
					VkCheck(vkCreateCommandPool(vkDevice, &ci, NULL, &tl.frameCommandPools[i][j]));
				}
			}
		}
	}
	// Create swapchain.
	{
		auto surfCaps = VkSurfaceCapabilitiesKHR{};
		VkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkPhysicalDevice, vkSurface, &surfCaps));
		auto ext = VkExtent2D{};
		if (surfCaps.currentExtent.width == U32Max && surfCaps.currentExtent.height == U32Max) // Indicates Vulkan will accept any extent dimension.
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
			Abort("Vulkan", "Swapchain image dimensions do not match the window dimensions: swapchain %ux%u, window %ux%u.", ext.width, ext.height, RenderWidth(), RenderHeight());
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
			.preTransform = surfCaps.currentTransform,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = vkPresentMode,
			.clipped = 1,
			.oldSwapchain = NULL,
		};
		if (vkPresentQueueFamily != vkQueueFamilies[GPUGraphicsQueue])
		{
			auto fams = MakeStaticArray<u32>(
				vkQueueFamilies[GPUGraphicsQueue],
				vkPresentQueueFamily);
			ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			ci.queueFamilyIndexCount = fams.Count();
			ci.pQueueFamilyIndices = fams.elements;
		}
		else
		{
			ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}
		VkCheck(vkCreateSwapchainKHR(vkDevice, &ci, NULL, &vkSwapchain));
	}
	// Get swapchain images.
	{
		auto numImgs = u32{};
		VkCheck(vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &numImgs, NULL));
		vkSwapchainImages.Resize(numImgs);
		VkCheck(vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &numImgs, &vkSwapchainImages[0]));
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
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
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
			VkCheck(vkCreateImageView(vkDevice, &ci, NULL, &vkSwapchainImageViews[i]));
		}
	}
}

VkCommandBuffer NewVulkanCommandBuffer(GPUQueueType t, VkCommandPool p)
{
    auto ai = VkCommandBufferAllocateInfo
    {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = p,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
    };
    auto cb = VkCommandBuffer{};
    vkAllocateCommandBuffers(vkDevice, &ai, &cb);
	auto bi = VkCommandBufferBeginInfo
	{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(cb, &bi);
	return cb;
}

GPUFrameCommandBuffer NewGPUFrameCommandBuffer(GPUQueueType t)
{
	auto cb = GPUFrameCommandBuffer{};
	cb.type = t;
    cb.vkCommandBuffer = NewVulkanCommandBuffer(t, vkThreadLocal[ThreadIndex()].frameCommandPools[vkFrameIndex][t]);
	//DisableJobSwitching();
	return cb;
}

// @TODO: Move queue type into type.
//        Move buffer type into name.
//        Move present queue in line with other queues.

GPUAsyncCommandBuffer NewGPUAsyncCommandBuffer(GPUQueueType t)
{
	auto cb = GPUAsyncCommandBuffer{};
	cb.type = t;
	vkThreadLocal[ThreadIndex()].asyncCommandBufferLock.Lock();
	Defer(vkThreadLocal[ThreadIndex()].asyncCommandBufferLock.Unlock());
	cb.vkCommandBuffer = NewVulkanCommandBuffer(t, vkThreadLocal[ThreadIndex()].asyncCommandPools[t]);
	//DisableJobSwitching();
	return cb;
}

void GPUCommandBuffer::BeginRenderPass(GPURenderPass rp, GPUFramebuffer fb)
{
	auto clearColor = VkClearValue
	{
		.color.float32 = {0.04f, 0.19f, 0.34f, 1.0f},
	};
	auto clearDepthStencil = VkClearValue
	{
		.depthStencil.depth = 1.0f,
		.depthStencil.stencil = 0,
	};
	auto clearValues = MakeStaticArray<VkClearValue>(clearColor, clearDepthStencil);
	auto bi = VkRenderPassBeginInfo
	{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = rp.vkRenderPass,
		.framebuffer = fb.vkFramebuffer,
		.renderArea =
		{
			.offset = {0, 0},
			.extent = {(u32)RenderWidth(), (u32)RenderHeight()},
		},
		.clearValueCount = (u32)clearValues.Count(),
		.pClearValues = clearValues.elements,
	};
	vkCmdBeginRenderPass(this->vkCommandBuffer, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

void GPUCommandBuffer::EndRenderPass()
{
	vkCmdEndRenderPass(this->vkCommandBuffer);
}

void GPUCommandBuffer::SetViewport(s64 w, s64 h)
{
	auto v = VkViewport
	{
		.x = 0.0f,
		.y = 0.0f,
		.width = (f32)w,
		.height = (f32)h,
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	vkCmdSetViewport(this->vkCommandBuffer, 0, 1, &v);
}

void GPUCommandBuffer::SetScissor(s64 w, s64 h)
{
	auto scissor = VkRect2D
	{
		.extent = {(u32)w, (u32)h},
	};
	vkCmdSetScissor(this->vkCommandBuffer, 0, 1, &scissor);
}

void GPUCommandBuffer::BindPipeline(GPUPipeline p)
{
	vkCmdBindPipeline(this->vkCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, p.vkPipeline);
}

void GPUCommandBuffer::BindVertexBuffer(GPUBuffer b, s64 bindPoint)
{
	auto offset = VkDeviceSize{0};
	vkCmdBindVertexBuffers(this->vkCommandBuffer, bindPoint, 1, &b.vkBuffer, &offset);
}

void GPUCommandBuffer::BindIndexBuffer(GPUBuffer b)
{
	vkCmdBindIndexBuffer(this->vkCommandBuffer, b.vkBuffer, 0, VK_INDEX_TYPE_UINT32);
}

/*
template <GPUSync S>
void GPUCommandBuffer<S>::BindDescriptors(ArrayView<GPUDescriptors> ds)
{
	// @TODO
}
*/

void GPUCommandBuffer::DrawIndexedVertices(s64 numIndices, s64 firstIndex, s64 vertexOffset)
{
	vkCmdDrawIndexed(this->vkCommandBuffer, numIndices, 1, firstIndex, vertexOffset, 0);
}

void GPUCommandBuffer::CopyBuffer(s64 size, GPUBuffer src, GPUBuffer dst, s64 srcOffset, s64 dstOffset)
{
	auto c = VkBufferCopy
	{
		.srcOffset = (VkDeviceSize)srcOffset,
		.dstOffset = (VkDeviceSize)dstOffset,
		.size = (VkDeviceSize)size,
	};
	vkCmdCopyBuffer(this->vkCommandBuffer, src.vkBuffer, dst.vkBuffer, 1, &c);
}

void GPUCommandBuffer::CopyBufferToImage(GPUBuffer b, GPUImage i, u32 w, u32 h)
{
	auto region = VkBufferImageCopy
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
			w,
			h,
			1,
		},
	};
	vkCmdCopyBufferToImage(this->vkCommandBuffer, b.vkBuffer, i.vkImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void GPUFrameCommandBuffer::Queue()
{
	VkCheck(vkEndCommandBuffer(this->vkCommandBuffer));
	vkThreadLocal[CurrentThread()].frameQueuedCommandBuffers[vkFrameIndex][this->type].Append(this->vkCommandBuffer);
}

void GPUAsyncCommandBuffer::Queue(bool *signalOnCompletion)
{
	VkCheck(vkEndCommandBuffer(this->vkCommandBuffer));
	vkThreadLocal[CurrentThread()].asyncCommandBufferLock.Lock();
	Defer(vkThreadLocal[CurrentThread()].asyncCommandBufferLock.Unlock());
	vkThreadLocal[CurrentThread()].asyncQueuedCommandBuffers[this->type].Append(this->vkCommandBuffer);
	vkThreadLocal[CurrentThread()].asyncSignals[this->type].Append(signalOnCompletion);
}

GPUFence SubmitFrameGPUCommandBuffers(GPUQueueType t, ArrayView<GPUSemaphore> waitSems, ArrayView<GPUPipelineStageFlags> waitStages, ArrayView<GPUSemaphore> signalSems)
{
	auto cbs = &vkThreadLocal[ThreadIndex()].frameQueuedCommandBuffers[vkFrameIndex][t];
	auto si = VkSubmitInfo
	{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = (u32)waitSems.count,
		.pWaitSemaphores = (VkSemaphore *)waitSems.elements,
		.pWaitDstStageMask = waitStages.elements,
		.commandBufferCount = (u32)cbs->count,
		.pCommandBuffers = cbs->elements,
		.signalSemaphoreCount = (u32)signalSems.count,
		.pSignalSemaphores = (VkSemaphore *)signalSems.elements,
	};
	auto f = NewGPUFence();
	VkCheck(vkQueueSubmit(vkQueues[t], 1, &si, f.vkFence));
	return f;
}

GPUFence SubmitAsyncGPUCommandBuffers(GPUQueueType t)
{
	return {};
}

#if 0
VkBufferUsageFlags GPUBufferTypeToVulkanBufferUsageFlag(GPUBufferType t)
{
	switch (t)
	{
	case GPUVertexBuffer:
	{
		return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	} break;
	case GPUIndexBuffer:
	{
		return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	} break;
	case GPUUniformBuffer:
	{
		return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	} break;
	case GPUTransferBuffer
	{
	} break;
	case GPUBufferTypeCount:
	default:
	{
		Abort("Vulkan", "Unknown buffer type %d.", t);
	} break;
	}
	return VkBufferUsageFlag{};
}
#endif

VkBuffer NewVulkanBuffer(GPUBufferType t, s64 size)
{
	auto b = VkBuffer{};
	auto ci = VkBufferCreateInfo
	{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = (VkDeviceSize)size,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkCheck(vkCreateBuffer(vkDevice, &ci, NULL, &b));
	return b;
}

VkBuffer NewFrameVulkanBuffer(GPUBufferType t, s64 size)
{
	auto b = vkThreadLocal[ThreadIndex()].frameBufferPool[vkFrameIndex][t].GetValue();
	if (!b)
	{
		b = NewVulkanBuffer(t, size);
	}
	return b;
}

VkBuffer NewAsyncVulkanBuffer(GPUBufferType t, s64 size)
{
	auto b = vkThreadLocal[ThreadIndex()].asyncBufferPool[t].Get();
	if (!b)
	{
		b = NewVulkanBuffer(t, size);
	}
	return b;
}

GPUBuffer NewGPUBuffer(GPUBufferType t, s64 size)
{
	vkThreadLocal[ThreadIndex()].asyncBufferLock.Lock();
	Defer(vkThreadLocal[ThreadIndex()].asyncBufferLock.Unlock());
	auto b = GPUBuffer
	{
		.type = t,
		.vkBuffer = NewAsyncVulkanBuffer(t, size),
	};
	auto mr = VkMemoryRequirements{};
	vkGetBufferMemoryRequirements(vkDevice, b.vkBuffer, &mr);
	auto err = false;
	b.memory = vkGPUBlockAllocator.Allocate(mr.size, mr.alignment);
	// @TODO: Get an error from the allocator and handle it.
	VkCheck(vkBindBufferMemory(vkDevice, b.vkBuffer, b.memory->vkMemory, b.memory->offset));
	return b;
}

void GPUBuffer::Free()
{
	this->memory->Free();
	vkThreadLocal[ThreadIndex()].asyncBufferLock.Lock();
	Defer(vkThreadLocal[ThreadIndex()].asyncBufferLock.Unlock());
	vkThreadLocal[ThreadIndex()].asyncBufferPool[this->type].Release(this->vkBuffer);
}

GPUFrameStagingBuffer NewGPUFrameStagingBuffer(s64 size, GPUBuffer dst)
{
	auto sb = GPUFrameStagingBuffer
	{
		.size = size,
		.vkBuffer = NewFrameVulkanBuffer(GPUTransferBuffer, size),
		.vkDestinationBuffer = dst.vkBuffer,
	};
	auto mr = VkMemoryRequirements{};
	vkGetBufferMemoryRequirements(vkDevice, sb.vkBuffer, &mr);
	auto mem = vkCPUToGPUFrameAllocator.Allocate(mr.size, mr.alignment, vkFrameIndex);
	VkCheck(vkBindBufferMemory(vkDevice, sb.vkBuffer, mem->vkMemory, mem->offset));
	return sb;
}

void GPUFrameStagingBuffer::Flush()
{
	auto src = GPUBuffer
	{
		.vkBuffer = this->vkBuffer,
	};
	auto dst = GPUBuffer
	{
		.vkBuffer = this->vkDestinationBuffer,
	};
	auto cb = NewGPUFrameCommandBuffer(GPUTransferQueue);
	cb.CopyBuffer(this->size, src, dst, 0, 0);
	cb.Queue();
}

GPUAsyncStagingBuffer NewGPUAsyncStagingBuffer(s64 size, GPUBuffer dst)
{
	auto sb = GPUAsyncStagingBuffer
	{
		.vkBuffer = NewAsyncVulkanBuffer(GPUTransferBuffer, size),
		.vkDestinationBuffer = dst.vkBuffer,
	};
	auto mr = VkMemoryRequirements{};
	vkGetBufferMemoryRequirements(vkDevice, sb.vkBuffer, &mr);
	sb.memory = vkCPUToGPUBlockAllocator.Allocate(mr.size, mr.alignment);
	VkCheck(vkBindBufferMemory(vkDevice, sb.vkBuffer, sb.memory->vkMemory, sb.memory->offset));
	return sb;
}

void GPUAsyncStagingBuffer::Flush()
{
	auto src = GPUBuffer
	{
		.vkBuffer = this->vkBuffer,
	};
	auto dst = GPUBuffer
	{
		.vkBuffer = this->vkDestinationBuffer,
	};
	auto cb = NewGPUAsyncCommandBuffer(GPUTransferQueue);
	cb.CopyBuffer(this->memory->size, src, dst, 0, 0);
	auto tl = &vkThreadLocal[ThreadIndex()];
	tl->asyncStagingBufferLock.Lock();
	Defer(tl->asyncStagingBufferLock.Unlock());
	tl->asyncStagingSignals.Resize(tl->asyncStagingSignals.count + 1);
	cb.Queue(&tl->asyncStagingSignals[tl->asyncStagingSignals.count - 1]);
	tl->asyncPendingStagingBuffers.Append({this->memory, this->vkBuffer});
}

GPUFence NewGPUFence()
{
	auto f = vkThreadLocal[ThreadIndex()].fencePool.Get();
	if (!f)
	{
		auto ci = VkFenceCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		};
		VkCheck(vkCreateFence(vkDevice, &ci, NULL, &f));
	}
	return GPUFence{f};
}

bool GPUFence::Check()
{
	auto s = vkGetFenceStatus(vkDevice, this->vkFence);
	if (s == VK_SUCCESS)
	{
		return true;
	}
	if (s == VK_NOT_READY)
	{
		return false;
	}
	VkCheck(s);
	return false;
}

bool GPUFence::Wait(u64 timeout)
{
	auto s = vkWaitForFences(vkDevice, 1, &this->vkFence, true, timeout);
	if (s == VK_SUCCESS)
	{
		return true;
	}
	if (s == VK_TIMEOUT)
	{
		return false;
	}
	VkCheck(s);
	return false;
}

void GPUFence::Free()
{
	VkCheck(vkResetFences(vkDevice, 1, &this->vkFence));
	vkThreadLocal[ThreadIndex()].fencePool.Release(this->vkFence);
}

bool WaitForFences(ArrayView<GPUFence> fs, bool waitAll, u64 timeout)
{
	auto s = vkWaitForFences(vkDevice, fs.count, (VkFence *)fs.elements, waitAll, timeout);
	if (s == VK_SUCCESS)
	{
		return true;
	}
	if (s == VK_TIMEOUT)
	{
		return false;
	}
	VkCheck(s);
	return false;
}

void FreeGPUFences(ArrayView<GPUFence> fs)
{
	VkCheck(vkResetFences(vkDevice, fs.count, (VkFence *)fs.elements));
	for (auto f : fs)
	{
		vkThreadLocal[ThreadIndex()].fencePool.Release(f.vkFence);
	}
}

void StartGPUFrame()
{
	VkCheck(vkWaitForFences(vkDevice, 1, &vkFrameFences[vkFrameIndex], true, U32Max));
	VkCheck(vkResetFences(vkDevice, 1, &vkFrameFences[vkFrameIndex]));
	VkCheck(vkAcquireNextImageKHR(vkDevice, vkSwapchain, U64Max, vkImageAcquiredSemaphores[vkFrameIndex], NULL, &vkSwapchainImageIndex));
	for (auto &tl : vkThreadLocal)
	{
		for (auto i = 0; i < GPUQueueTypeCount; i += 1)
		{
			tl.frameBufferPool[vkFrameIndex][i].Reset();
			tl.frameCommandBufferPool[vkFrameIndex][i].Reset();
			VkCheck(vkResetCommandPool(vkDevice, tl.frameCommandPools[vkFrameIndex][i], 0));
		}
		tl.asyncCommandBufferLock.Lock();
		for (auto i = 0; i < GPUQueueTypeCount; i += 1)
		{
			for (auto j = 0; j < tl.asyncFences[i].count; )
			{
				if (!tl.asyncFences[i][j].Check())
				{
					j += 1;
					continue;
				}
				tl.asyncFences[i][j].Free();
				tl.asyncFences[i].UnorderedRemove(j);
				*tl.asyncSignals[i][j] = true;
				tl.asyncSignals[i].UnorderedRemove(j);
				tl.asyncCommandBufferPool[i].ReleaseAll(tl.asyncPendingCommandBuffers[i][j]);
				tl.asyncPendingCommandBuffers[i][j].Resize(0);
			}
		}
		tl.asyncCommandBufferLock.Unlock();
		tl.asyncStagingBufferLock.Lock();
		for (auto i = 0; i < tl.asyncStagingSignals.count; )
		{
			if (!tl.asyncStagingSignals[i])
			{
				i += 1;
				continue;
			}
			// @TODO: Take locks...
			tl.asyncStagingSignals.UnorderedRemove(i);
			tl.asyncPendingStagingBuffers[i].memory->Free();
			tl.asyncBufferPool[GPUTransferQueue].Release(tl.asyncPendingStagingBuffers[i].vkBuffer);
			tl.asyncPendingStagingBuffers.UnorderedRemove(i);
		}
		tl.asyncStagingBufferLock.Unlock();
	}
	vkCPUToGPUFrameAllocator.Free(vkFrameIndex);
}

void FinishGPUFrame()
{
	vkFrameIndex = (vkFrameIndex + 1) % VulkanMaxFramesInFlight;
}

#endif
