#include "../Memory.h"
#include "../Assert.h"
#include "../Log.h"
#include "../PCH.h"

#define MAP_ANONYMOUS 0x20

void *AllocatePlatformMemory(s64 size)
{
	auto memory = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == (void *)-1)
	{
		Assert(0); // @TODO
	}
	return memory;
}

void *AllocateAlignedMemory(s64 alignment, s64 size)
{
	auto result = aligned_alloc(alignment, size);
	Assert(result);
	return result;
}

void FreePlatformMemory(void *memory, s64 size)
{
	if (munmap(memory, size) == -1)
	{
		LogPrint(ERROR_LOG, "Failed to free platform memory: %s.\n", GetPlatformError());
	}
}

size_t GetPageSize()
{
	return sysconf(_SC_PAGESIZE);
}
