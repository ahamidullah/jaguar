#pragma once

#include "Memory/ContextAllocator.h"
#include "Assert.h"
#include "Common.h"

template <typename T> struct Array;
template <typename T> struct ArrayView;

template <typename T>
s64 ArrayFindFirst(ArrayView<T> a, T e)
{
	for (auto i = 0; i < a.count; i += 1)
	{
		if (a[i] == e)
		{
			return i;
		}
	}
	return -1;
}

template <typename T>
s64 ArrayFindLast(ArrayView<T> a, T e)
{
	auto last = -1;
	for (auto i = 0; i < a.count; i += 1)
	{
		if (a[i] == e)
		{
			last = i;
		}
	}
	return last;
}

template <typename T>
struct ArrayView
{
	T *elements;
	s64 count;

	T &operator[](s64 i);
	const T &operator[](s64 i) const;
	bool operator==(ArrayView<T> a);
	bool operator!=(ArrayView<T> a);
	T *begin();
	T *end();
	ArrayView<u8> Bytes();
	ArrayView<T> View(s64 start, s64 end);
	Array<T> Copy();
	Array<T> CopyIn(Memory::Allocator *a);
	Array<T> CopyRange(s64 start, s64 end);
	Array<T> CopyRangeIn(Memory::Allocator *a, s64 start, s64 end);
	s64 FindFirst(T e);
	s64 FindLast(T e);
};

template <typename T>
ArrayView<T> NewArrayView(T *data, s64 count)
{
	return
	{
		.elements = data,
		.count = count,
	};
}

template <typename T>
T &ArrayView<T>::operator[](s64 i)
{
	Assert(i >= 0 && i < count);
	return elements[i];
}

template <typename T>
const T &ArrayView<T>::operator[](s64 i) const
{
	Assert(i >= 0 && i < count);
	return elements[i];
}

template <typename T>
bool ArrayView<T>::operator==(ArrayView<T> a)
{
	if (this->count != a.count)
	{
		return false;
	}
	for (auto i = 0; i < this->count; i += 1)
	{
		if ((*this)[i] != a[i])
		{
			return false;
		}
	}
	return true;
}

template <typename T>
bool ArrayView<T>::operator!=(ArrayView<T> a)
{
	return !(*this == a);
}

template <typename T>
T *ArrayView<T>::begin()
{
	return &this->elements[0];
}

template <typename T>
T *ArrayView<T>::end()
{
	return &this->elements[this->count - 1] + 1;
}

template <typename T>
ArrayView<u8> ArrayView<T>::Bytes()
{
	return
	{
		.elements = (u8 *)this->elements,
		.count = this->count * sizeof(T),
	};
}

template <typename T>
ArrayView<T> ArrayView<T>::View(s64 start, s64 end)
{
	Assert(start <= end);
	Assert(start >= 0);
	Assert(end <= this->count);
	return
	{
		.elements = &this->elements[start],
		.count = end - start,
	};
}

template <typename T> Array<T> NewArray(s64 count);

template <typename T>
Array<T> ArrayView<T>::Copy()
{
	return this->CopyIn(Memory::ContextAllocator());
}

template <typename T>
Array<T> ArrayView<T>::CopyIn(Memory::Allocator *a)
{
	auto r = NewArrayIn<T>(a, this->count);
	CopyArray(*this, r);
	return r;
}

template <typename T>
Array<T> ArrayView<T>::CopyRange(s64 start, s64 end)
{
	this->CopyRangeIn(Memory::ContextAllocator(), start, end);
}

template <typename T>
Array<T> ArrayView<T>::CopyRangeIn(Memory::Allocator *a, s64 start, s64 end)
{
	Assert(end >= start);
	auto ar = NewArrayIn<T>(a, end - start);
	CopyArray(this->View(start, end), ar);
	return ar;
}

template <typename T>
s64 ArrayView<T>::FindFirst(T e)
{
	return ArrayFindFirst<T>(*this, e);
}

template <typename T>
s64 ArrayView<T>::FindLast(T e)
{
	return ArrayFindLast<T>(*this, e);
}

// Implicit type conversion does not work with template functions, so we'll have to create overloads of CopyArray for each combination of Array, StaticArray, and ArrayView.
// C++ sucks!

template <typename T>
void CopyArray(Array<T> src, ArrayView<T> dst)
{
	Assert(src.count == dst.count);
	for (auto i = 0; i < src.count; i += 1)
	{
		dst[i] = src[i];
	}
}

template <typename T>
void CopyArray(Array<T> src, Array<T> dst)
{
	Assert(src.count == dst.count);
	for (auto i = 0; i < src.count; i += 1)
	{
		dst[i] = src[i];
	}
}

template <typename T>
void CopyArray(ArrayView<T> src, Array<T> dst)
{
	Assert(src.count == dst.count);
	for (auto i = 0; i < src.count; i += 1)
	{
		dst[i] = src[i];
	}
}

template <typename T>
void CopyArray(ArrayView<T> src, ArrayView<T> dst)
{
	Assert(src.count == dst.count);
	for (auto i = 0; i < src.count; i += 1)
	{
		dst[i] = src[i];
	}
}

template <typename T>
struct Array
{
	Memory::Allocator *allocator;
	T *elements;
	s64 count;
	s64 capacity;

	operator ArrayView<T>();
	T &operator[](s64 i);
	const T &operator[](s64 i) const;
	bool operator==(Array<T> a);
	bool operator!=(Array<T> a);
	T *begin();
	T *end();
	void Free();
	void SetAllocator(Memory::Allocator *a);
	void Reserve(s64 reserve);
	void Resize(s64 count);
	void Append(T e);
	void AppendAll(ArrayView<T> a);
	void OrderedRemove(s64 index);
	void UnorderedRemove(s64 index);
	typedef bool (*SiftProcedure)(T e);
	ArrayView<T> Sift(SiftProcedure p);
	Array<T> Copy();
	Array<T> CopyIn(Memory::Allocator *a);
	Array<T> CopyRange(s64 start, s64 end);
	Array<T> CopyRangeIn(Memory::Allocator *a, s64 start, s64 end);
	ArrayView<T> View(s64 start, s64 end);
	ArrayView<u8> Bytes();
	s64 FindFirst(T e);
	s64 FindLast(T e);
	T Pop();
	T *Last();
};

template <typename T> Array<T> NewArrayWithCapacityIn(Memory::Allocator *a, s64 cap);

template <typename T, typename... Ts>
Array<T> MakeArrayIn(Memory::Allocator *a, Ts... ts)
{
	auto r = NewArrayWithCapacityIn<T>(a, sizeof...(ts));
	(r.Append(ts), ...);
	return r;
}

template <typename T, typename... Ts>
Array<T> MakeArray(Ts... ts)
{
	return MakeArrayIn<T>(Memory::ContextAllocator(), ts...);
}

template <typename T>
Array<T> NewArrayIn(Memory::Allocator *a, s64 count)
{
	return
	{
		.allocator = a,
		.elements = (T *)a->Allocate(count * sizeof(T)),
		.count = count,
		.capacity = count,
	};
}

template <typename T>
Array<T> NewArray(s64 count)
{
	return NewArrayIn<T>(Memory::ContextAllocator(), count);
}

template <typename T>
Array<T> NewArrayWithCapacityIn(Memory::Allocator *a, s64 cap)
{
	return
	{
		.allocator = a,
		.elements = (T *)a->Allocate(cap * sizeof(T)),
		.capacity = cap,
	};
}

template <typename T>
Array<T> NewArrayWithCapacity(s64 cap)
{
	return NewArrayWithCapacityIn<T>(Memory::ContextAllocator(), cap);
}

template <typename T>
Array<T>::operator ArrayView<T>()
{
	return ArrayView<T>
	{
		.elements = elements,
		.count = count, 
	};
}

template <typename T>
T &Array<T>::operator[](s64 i)
{
	Assert(i >= 0 && i < this->count);
	return this->elements[i];
}

template <typename T>
const T &Array<T>::operator[](s64 i) const
{
	Assert(i >= 0 && i < this->count);
	return this->elements[i];
}

template <typename T>
bool Array<T>::operator==(Array<T> a)
{
	if (this->count != a.count)
	{
		return false;
	}
	for (auto i = 0; i < this->count; i += 1)
	{
		if ((*this)[i] != a[i])
		{
			return false;
		}
	}
	return true;
}

template <typename T>
bool Array<T>::operator!=(Array<T> a)
{
	return !(*this == a);
}

template <typename T>
T *Array<T>::begin()
{
	return &this->elements[0];
}

template <typename T>
T *Array<T>::end()
{
	return &this->elements[this->count - 1] + 1;
}

template <typename T>
Array<T> Array<T>::Copy()
{
	return this->CopyIn(Memory::ContextAllocator());
}

template <typename T>
Array<T> Array<T>::CopyIn(Memory::Allocator *a)
{
	auto r = NewArrayIn<T>(a, this->count);
	CopyArray(*this, r);
	return r;
}

template <typename T>
Array<T> Array<T>::CopyRange(s64 start, s64 end)
{
	return this->CopyRangeIn(Memory::ContextAllocator(), start, end);
}

template <typename T>
Array<T> Array<T>::CopyRangeIn(Memory::Allocator *a, s64 start, s64 end)
{
	Assert(end >= start);
	auto r = NewArrayIn<T>(a, end - start);
	CopyArray(this->View(start, end), r);
	return r;
}

template <typename T>
ArrayView<u8> Array<T>::Bytes()
{
	return
	{
		.elements = (u8 *)this->elements,
		.count = this->count * sizeof(T),
	};
}

template <typename T>
ArrayView<T> Array<T>::View(s64 start, s64 end)
{
	Assert(start <= end);
	Assert(start >= 0);
	Assert(end <= this->count);
	return
	{
		.elements = &this->elements[start],
		.count = end - start,
	};
}

template <typename T>
s64 Array<T>::FindFirst(T e)
{
	return ArrayFindFirst<T>(*this, e);
}

template <typename T>
s64 Array<T>::FindLast(T e)
{
	return ArrayFindLast<T>(*this, e);
}

template <typename T>
void Array<T>::SetAllocator(Memory::Allocator *a)
{
	this->allocator = a;
}

template <typename T>
void Array<T>::Reserve(s64 n)
{
	if (!this->allocator)
	{
		this->allocator = Memory::ContextAllocator();
	}
	if (!this->elements)
	{
		this->elements = (T *)this->allocator->Allocate(n * sizeof(T));
		this->capacity = n;
		return;
	}
	if (this->capacity >= n)
	{
		return;
	}
	if (this->capacity == 0)
	{
		this->capacity = 1;
	}
	while (this->capacity < n)
	{
		this->capacity = (this->capacity * 2);
	}
	this->elements = (T *)this->allocator->Resize(this->elements, this->capacity * sizeof(T));
}

template <typename T>
void Array<T>::Resize(s64 n)
{
	this->Reserve(n);
	this->count = n;
}

template <typename T>
void Array<T>::Append(T e)
{
	auto i = this->count;
	this->Resize(this->count + 1);
	Assert(this->count <= this->capacity);
	Assert(i < this->capacity);
	(*this)[i] = e;
}

template <typename T>
void Array<T>::AppendAll(ArrayView<T> a)
{
	auto oldCount = this->count;
	auto newCount = this->count + a.count;
	this->Resize(newCount);
	CopyArray(a, this->View(oldCount, newCount));
}

template <typename T>
void Array<T>::OrderedRemove(s64 i)
{
	Assert(i > 0 && i < this->count);
	CopyArray(this->View(i + 1, this->count), this->View(i, this->count - 1));
	this->count -= 1;
}

template <typename T>
void Array<T>::UnorderedRemove(s64 i)
{
	Assert(i > 0 && i < this->count);
	this->elements[i] = this->elements[this->count - 1];
	this->count -= 1;
}

template <typename T>
ArrayView<T> Array<T>::Sift(SiftProcedure p)
{
	auto numValid = 0, numInvalid = 0;
	auto i = 0;
	auto count = this->count;
	while (count > 0)
	{
		if (!p((*this)[i]))
		{
			(*this)[i] = (*this)[this->count - 1 - numInvalid];
			numInvalid += 1;
		}
		else
		{
			numValid += 1;
			i += 1;
		}
		count -= 1;
	}
	return
	{
		.elements = (*this)[0],
		.count = numValid,
	};
}

template <typename T>
void Array<T>::Free()
{
	if (!this->elements)
	{
		return;
	}
	this->allocator->Deallocate(this->elements);
	this->count = 0;
	this->capacity = 0;
	this->elements = NULL;
}

template <typename T>
T Array<T>::Pop()
{
	Assert(this->count > 0);
	this->Resize(this->count - 1);
	return this->elements[this->count];
}

template <typename T>
T *Array<T>::Last()
{
	return &(*this)[this->count - 1];
}

template <typename T, s64 N>
struct StaticArray
{
	T elements[N];

	operator ArrayView<T>();
	T &operator[](s64 i);
	const T &operator[](s64 i) const;
	bool operator==(StaticArray<T, N> a);
	bool operator!=(StaticArray<T, N> a);
	T *begin();
	T *end();
	s64 Count();
	ArrayView<T> View(s64 start, s64 end);
	ArrayView<u8> Bytes();
	s64 FindFirst(T e);
	s64 FindLast(T e);
};

/*
	THIS SUCKS
		auto vertAttrs = MakeStaticArray(
			VkVertexInputAttributeDescription
			{
				.location = 0,
				.binding = VulkanVertexBufferBindID,
				.format = VK_FORMAT_R32G32B32_SFLOAT,
				.offset = offsetof(Vertex1P1N, position),
			},
			VkVertexInputAttributeDescription
			{
				.location = 1,
				.binding = VulkanVertexBufferBindID,
				.format = VK_FORMAT_R32G32B32_SFLOAT,
				.offset = offsetof(Vertex1P1N, normal),
			});
*/
template <typename T, typename... Ts>
StaticArray<T, sizeof...(Ts) + 1> MakeStaticArray(T t, Ts... ts)
{
	return
	{
		t,
		ts...,
	};
}

template <typename T, s64 N>
StaticArray<T, N>::operator ArrayView<T>()
{
	return ArrayView<T>
	{
		.elements = this->elements,
		.count = N,
	};
}

template <typename T, s64 N>
T &StaticArray<T, N>::operator[](s64 i)
{
	Assert(i >= 0 && i < N);
	return this->elements[i];
}

template <typename T, s64 N>
const T &StaticArray<T, N>::operator[](s64 i) const
{
	Assert(i >= 0 && i < N);
	return this->elements[i];
}

template <typename T, s64 N>
bool StaticArray<T, N>::operator==(StaticArray<T, N> a)
{
	if (this->count != a.count)
	{
		return false;
	}
	for (auto i = 0; i < N; i += 1)
	{
		if ((*this)[i] != a[i])
		{
			return false;
		}
	}
	return true;
}

template <typename T, s64 N>
bool StaticArray<T, N>::operator!=(StaticArray<T, N> a)
{
	return !(*this == a);
}

template <typename T, s64 N>
T *StaticArray<T, N>::begin()
{
	return &this->elements[0];
}

template <typename T, s64 N>
T *StaticArray<T, N>::end()
{
	return &this->elements[N - 1] + 1;
}

template <typename T, s64 N>
s64 StaticArray<T, N>::Count()
{
	return N;
}

template <typename T, s64 N>
ArrayView<T> StaticArray<T, N>::View(s64 start, s64 end)
{
	Assert(start <= end);
	Assert(start >= 0);
	Assert(end <= this->count);
	return
	{
		.elements = &this->elements[start],
		.count = end - start,
	};
}

template <typename T, s64 N>
ArrayView<u8> StaticArray<T, N>::Bytes()
{
	return
	{
		.elements = (u8 *)this->elements,
		.count = N * sizeof(T),
	};
}

template <typename T, s64 N>
s64 StaticArray<T, N>::FindFirst(T e)
{
	return ArrayFindFirst(*this, e);
}

template <typename T, s64 N>
s64 StaticArray<T, N>::FindLast(T e)
{
	return ArrayFindLast<T>(*this, e);
}
