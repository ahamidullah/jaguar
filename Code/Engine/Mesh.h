#pragma once

#include "Math.h"
#include "Transform.h"

enum VertexFormat
{
	VertexFormat1P,
	VertexFormat1P1N,
	VertexFormat1P1C1UV1N1T,
};

struct Vertex1P1C1UV1N1T
{
	V3 position;
	V3 color;
	V2 uv;
	V3 normal;
	V3 tangent;
};

struct Vertex1P
{
	V3 position;
};

struct Vertex1P1N
{
	V3 position;
	//int padding[1];
	V3 normal;
};

struct SubmeshAsset
{
	u32 indexCount;
	u32 firstIndex;
	u32 vertexOffset;
};

struct MeshAsset
{
	u32 indexCount;
	//GPUIndexType indexType;
	// @TODO: All of this should probably be 32bit.
	GPU::Buffer vertexBuffer;
	GPU::Buffer indexBuffer;
	s64 vertexOffset;
	s64 firstIndex;
};

struct Mesh
{
	string::String meshName;
};
