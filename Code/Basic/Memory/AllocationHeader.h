#pragma once

#include "Common.h"

namespace Memory
{

struct AllocationHeader
{
	s64 size;
	s64 alignment;
};

u8 *SetAllocationHeaderAndData(void *mem, s64 size, s64 align);
AllocationHeader *GetAllocationHeader(void *mem);

}
