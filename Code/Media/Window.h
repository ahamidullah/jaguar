#pragma once

#if defined(__linux__)
	#include "Linux/Window.h"
#else
	#error Unsupported platform.
#endif