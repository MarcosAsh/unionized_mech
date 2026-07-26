#include "render_text.h"

#include "sim/sim.h"

#include <cstdio>

namespace render {

namespace {

// Each glyph is 5x7, drawn here as rows of '.' and '#'. The upload pads a
// one-texel transparent border, so the texture cell is 7x9.
struct GlyphArt {
    char ch;
    const char* rows[7];
};

constexpr GlyphArt FONT_ART[] = {
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", "..#..", "..#..", "..#.."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", ".#.#.", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'-', {".....", ".....", ".....", ".###.", ".....", ".....", "....."}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {':', {".....", "..#..", ".....", ".....", ".....", "..#..", "....."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".....", "..#.."}},
};

constexpr u32 CELL_W = 7;
constexpr u32 CELL_H = 9;
constexpr f32 GLYPH_ASPECT = static_cast<f32>(CELL_W) / static_cast<f32>(CELL_H);
constexpr f32 ADVANCE = 1.1f;  // spacing between glyph cells, in cell widths

}  // namespace

Font font_build(gpu::Renderer& gpu) {
    Font font{};
    u8 rgba[CELL_W * CELL_H * 4];
    for (const GlyphArt& art : FONT_ART) {
        for (u32 i = 0; i < sizeof(rgba); ++i) {
            rgba[i] = 0;
        }
        for (u32 row = 0; row < 7; ++row) {
            for (u32 col = 0; col < 5; ++col) {
                if (art.rows[row][col] != '#') {
                    continue;
                }
                u8* px = &rgba[(((row + 1) * CELL_W) + col + 1) * 4];
                px[0] = 255;
                px[1] = 255;
                px[2] = 255;
                px[3] = 255;
            }
        }
        font.glyphs[art.ch - 32] = gpu.create_texture(rgba, CELL_W, CELL_H).bindless_index;
    }
    return font;
}

f32 text_width(const char* text, f32 h, f32 aspect) {
    u32 count = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        ++count;
    }
    if (count == 0) {
        return 0.0f;
    }
    const f32 w = h * GLYPH_ASPECT / aspect;
    return (static_cast<f32>(count - 1) * ADVANCE + 1.0f) * w;
}

void text_queue(RenderModel& overlay, const Font& font, u32 slot, f32 x, f32 y, f32 h,
                f32 aspect, const char* text, const f32 tint[4]) {
    const f32 w = h * GLYPH_ASPECT / aspect;
    f32 cx = x + w * 0.5f;
    for (const char* p = text; *p != '\0'; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 32);
        }
        if (c > 32 && c < 127) {
            const u32 tex = font.glyphs[c - 32];
            if (tex != 0) {
                model_queue_glyph(overlay, slot, core::Vec3{cx, y + h * 0.5f, 0.0f},
                                  core::Vec3{w * 0.5f, h * 0.5f, 1.0f}, tint, tex);
            }
        }
        cx += w * ADVANCE;
    }
}

void hud_queue(RenderModel& overlay, const Font& font, u32 slot, const sim::World& world,
               f32 aspect) {
    constexpr f32 BLUE[4] = {0.40f, 0.65f, 1.00f, 0.92f};
    constexpr f32 ORANGE[4] = {1.00f, 0.55f, 0.28f, 0.92f};
    constexpr f32 WHITE[4] = {1.00f, 1.00f, 1.00f, 0.92f};
    constexpr f32 RED[4] = {1.00f, 0.25f, 0.20f, 0.95f};
    const sim::Character& me = world.player();
    char buf[32];

    // Team scores flank a dash at the top centre, each side in its colour.
    const f32 sh = 0.065f;
    std::snprintf(buf, sizeof(buf), "%u", sim::team_kills(world, 0));
    text_queue(overlay, font, slot, -0.045f - text_width(buf, sh, aspect), -0.97f, sh, aspect,
               buf, BLUE);
    text_queue(overlay, font, slot, -0.5f * text_width("-", sh, aspect), -0.97f, sh, aspect, "-",
               WHITE);
    std::snprintf(buf, sizeof(buf), "%u", sim::team_kills(world, 1));
    text_queue(overlay, font, slot, 0.045f, -0.97f, sh, aspect, buf, ORANGE);

    // Health bottom-left, kills bottom-right.
    std::snprintf(buf, sizeof(buf), "+%d", me.health > 0 ? me.health : 0);
    text_queue(overlay, font, slot, -0.96f, 0.87f, 0.08f, aspect, buf,
               me.health <= 25 ? RED : WHITE);
    std::snprintf(buf, sizeof(buf), "%u", static_cast<u32>(me.kills));
    text_queue(overlay, font, slot, 0.96f - text_width(buf, 0.08f, aspect), 0.87f, 0.08f, aspect,
               buf, WHITE);

    // Merged: the chassis core readout. Otherwise, prompt beside the mech.
    const sim::Mech& mech = world.mech;
    if (me.merged != 0) {
        std::snprintf(buf, sizeof(buf), "CORE %d", static_cast<int>(mech.health));
        text_queue(overlay, font, slot, -0.96f, 0.76f, 0.07f, aspect, buf,
                   mech.health <= 80 ? RED : WHITE);
    } else if (me.alive != 0 && mech.alive != 0 && mech.pilot == sim::NO_PILOT) {
        const f32 dx = me.x - mech.x;
        const f32 dz = me.z - mech.z;
        if (dx * dx + dz * dz < 49.0f) {
            const char* line = "E - UNION";
            text_queue(overlay, font, slot, -0.5f * text_width(line, 0.08f, aspect), 0.30f,
                       0.08f, aspect, line, WHITE);
        }
    }

    // Respawn countdown while dead; the winner banner overrides it.
    if (world.winner != 0) {
        const char* line = world.winner == 1 ? "BLUE WINS" : "ORANGE WINS";
        text_queue(overlay, font, slot, -0.5f * text_width(line, 0.16f, aspect), -0.30f, 0.16f,
                   aspect, line, world.winner == 1 ? BLUE : ORANGE);
    } else if (me.alive == 0) {
        std::snprintf(buf, sizeof(buf), "RESPAWN %u",
                      static_cast<u32>(me.respawn_ticks) / 60 + 1);
        text_queue(overlay, font, slot, -0.5f * text_width(buf, 0.10f, aspect), -0.20f, 0.10f,
                   aspect, buf, WHITE);
    }
}

}  // namespace render
