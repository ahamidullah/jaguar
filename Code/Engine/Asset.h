#pragma once

#include "Model.h"
#include "Basic/String.h"

enum AssetType
{
	ModelAssetType,
	AssetTypeCount
};

enum AssetID
{
	TriangleNoIndicesAssetID,
	AssetIDCount
};

struct LoadAssetJobParameters
{
	AssetID id;
	AssetType type;
};

void InitializeAssets(void *jobParameter);
void LoadAsset(string::String name);
void UnloadAsset(AssetID id);
void *LockAsset(AssetID id);
void UnlockAsset(AssetID id);
string::String AssetIDToString();
string::String AssetPath(AssetID id);
