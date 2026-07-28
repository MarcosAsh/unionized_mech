// The single home of third-party implementation expansions for the asset
// module. Compiled with warnings off, since the expanded code is not ours.
// Import work is offline tooling, so the libraries' internal use of malloc is
// outside the no-heap-in-frame-loop rule.

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#include <stb_image_write.h>
