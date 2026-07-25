#pragma once

// The Allocator and Allocation types moved to the public gpu.h so the Renderer
// can hold an Allocator by value. This header remains as the include point for
// the allocator implementation unit.

#include "gpu/gpu.h"
