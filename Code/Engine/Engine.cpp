#include "Engine.h"
#include "Render.h"
#include "Entity.h"
#include "Job.h"
#include "Camera.h"
#include "Media/Input.h"
#include "Basic/Process.h"
#include "Basic/Log.h"
#include "Basic/Time.h"

s64 windowWidth, windowHeight;

s64 WindowWidth()
{
	return windowWidth;
}

s64 WindowHeight()
{
	return windowHeight;
}

auto cam = (Camera *){};

void InitializeGameLoop()
{
	cam = NewCamera("Main", {2, 2, 2}, {0, 0, 0}, 1.4f, DegreesToRadians(90.0f));
	LoadAsset("Box");
	//auto e = NewEntity();
	//auto t = Transform{};
	//SetEntityTransform(e, t);
	//SetEntityModel(e, SponzaAssetID);
}

void GameLoop(f32 deltaTime)
{
	if (MouseDeltaX() != 0 || MouseDeltaY() != 0)
	{
		auto dPitch = -MouseDeltaY() * MouseSensitivity();
		auto dYaw = -MouseDeltaX() * MouseSensitivity();
		cam->transform.RotateEulerLocal({dPitch, dYaw, 0.0f});
	}
	if (IsKeyDown(AKey))
	{
		cam->transform.position -= cam->speed * cam->transform.Right();
	}
	else if (IsKeyDown(DKey))
	{
		cam->transform.position += cam->speed * cam->transform.Right();
	}
	if (IsKeyDown(QKey))
	{
		cam->transform.position.z -= cam->speed;
	}
	else if (IsKeyDown(EKey))
	{
		cam->transform.position.z += cam->speed;
	}
	if (IsKeyDown(WKey))
	{
		cam->transform.position += cam->speed * cam->transform.Forward();
	}
	else if (IsKeyDown(SKey))
	{
		cam->transform.position -= cam->speed * cam->transform.Forward();
	}
}

void Update()
{
	GameLoop(0.0f);
}

void Test2(void *x)
{
	LogInfo("Engine", "NUMBER2");
}

void Test(void *x)
{
	LogInfo("Engine", "HELLLLOOOOOO");
	static auto b = false;
	if (!b)
	{
		b = true;
	auto j = Array<JobDeclaration>{};
	for (auto i = 0; i < 100; i += 1)
	{
		j.Append(NewJobDeclaration(Test2, NULL));
	}
	auto c = (JobCounter *){};
	RunJobs(j, NormalJobPriority, &c);
	c->Wait();
	}
}

void RunGame(void *)
{
	windowWidth = RenderWidth();
	windowHeight = RenderHeight();
	auto win = NewWindow(windowWidth, windowHeight);
	LogInfo("Engine", "Window dimensions: %dx%d", windowWidth, windowHeight);
	InitializeRenderer(&win);
	InitializeAssets(NULL);
	InitializeEntities(); // @TODO
	InitializeGameLoop();
	#if 0
	auto j = Array<JobDeclaration>{};
	for (auto i = 0; i < 100; i += 1)
	{
		j.Append(NewJobDeclaration(Test, NULL));
	}
	auto c = (JobCounter *){};
	RunJobs(j, NormalJobPriority, &c);
	c->Wait();
	#endif
	auto t = NewTimer("Frame");
	while (true)
	{
		t.Print(MillisecondScale);
		t.Reset();
		auto winEvents = ProcessInput(&win);
		if (winEvents.quit)
		{
			break;
		}
		Update();
		Render();
	}
	ExitProcess(ProcessSuccess);
}

void LogBuildOptions()
{
	#ifdef DebugBuild
		LogInfo("Engine", "Speed: Debug");
	#else
		LogInfo("Engine", "Speed: Optimized");
	#endif
	#ifdef DevelopmentBuild
		LogInfo("Engine", "Runtime: Development");
	#else
		LogInfo("Engine", "Runtime: Release");
	#endif
	#ifdef VulkanBuild
		LogInfo("Engine", "Rendering API: Vulkan");
	#endif
	#ifdef AddressSanitizerBuild
		LogInfo("Engine", "Static Analysis: AddressSanitizer");
	#endif
	#ifdef ThreadSanitizerBuild
		LogInfo("Engine", "Static Analysis: ThreadSanitizer");
	#endif
}

s32 ApplicationEntry(s32 argc, char *argv[])
{
	LogBuildOptions();
	InitializeInput();
	InitializeJobs(RunGame, NULL);
	Abort("Engine", "Invalid exit from ApplicationEntry.");
	return ProcessFail;
}
