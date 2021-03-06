#include "Camera.h"
#include "Math.h"
#include "Basic/Container/Array.h"

// @TODO: Locking.

auto cameras = array::Array<Camera>{};

Camera *NewCamera(string::String name, V3 pos, V3 lookAt, f32 speed, f32 fov)
{
	auto c = Camera
	{
		.name = name, // @TODO: Copy or something?
		.pitch = 0.0f,
		.yaw = 0.0f,
		.roll = 0.0f,
		.transform =
		{
			.position = pos,
			.rotation = NewQuaternion(lookAt - pos),
		},
		.fov = fov,
		.focalLength = 0.01f,
		.speed = speed,
	};
	cameras.Append(c);
	return cameras.Last();
}

array::View<Camera> Cameras()
{
	return cameras;
}

Camera *LookupCamera(string::String name)
{
	for (auto i = 0; i < cameras.count; i += 1)
	{
		if (name == cameras[i].name)
		{
			return &cameras[i];
		}
	}
	return NULL;
}
