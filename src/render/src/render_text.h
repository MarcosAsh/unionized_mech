#pragma once

// Internal to the render module. A built-in 5x7 pixel font, one tiny bindless
// texture per glyph, drawn as records through the overlay pass. Enough for
// scores, health, and banners until real font assets exist.

#include "core/types.h"
#include "render_model.h"

namespace sim {
struct World;
}

namespace render {

/// Bindless texture per printable ASCII glyph (index is character minus 32);
/// zero marks a character the font does not carry.
struct Font {
    u32 glyphs[96];
};

/// Upload every glyph texture. Call once at scene creation.
[[nodiscard]] Font font_build(gpu::Renderer& gpu);

/// Width of `text` in NDC units at glyph height `h`. `aspect` is the
/// framebuffer's width over height, which keeps glyphs square on screen.
[[nodiscard]] f32 text_width(const char* text, f32 h, f32 aspect);

/// Queue `text` into the overlay model with its top-left corner at NDC
/// (x, y). Lowercase draws as uppercase; unknown characters advance silently.
void text_queue(RenderModel& overlay, const Font& font, u32 slot, f32 x, f32 y, f32 h,
                f32 aspect, const char* text, const f32 tint[4]);

/// Queue the whole match HUD: team scores, health, kills, the respawn
/// countdown, and the win banner.
void hud_queue(RenderModel& overlay, const Font& font, u32 slot, const sim::World& world,
               f32 aspect);

}  // namespace render
