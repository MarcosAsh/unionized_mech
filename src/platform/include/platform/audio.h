#pragma once

#include "core/result.h"
#include "core/types.h"

namespace platform {

/// The game's sound effects. All synthesized at open; there are no audio
/// assets to load.
enum class Sound : u8 {
    Fire,
    MechFire,
    Hit,
    Hurt,
    Death,
    Union,
    Boom,
    Win,
    Lose,
    Count,
};

/// Procedural audio out through the OS mixer. play() starts a voice, update()
/// keeps the output stream fed. Cosmetic and frame-loop-side only: nothing
/// here touches the simulation.
class Audio {
public:
    /// Open the default output device and synthesize the sound bank.
    /// # Errors
    /// A static message when no audio device is available. The game runs fine
    /// without one; a default-constructed Audio ignores every call.
    [[nodiscard]] static core::Result<Audio, const char*> open();

    Audio() = default;
    ~Audio();
    Audio(Audio&& other) noexcept;
    Audio& operator=(Audio&& other) noexcept;
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    /// Start `sound` at `gain` (0 to 1). Silently drops when out of voices.
    void play(Sound sound, f32 gain);

    /// Mix active voices and keep about 100ms queued on the device. Call once
    /// per frame; cheap when the queue is already full.
    void update();

private:
    struct Voice {
        u32 sound = 0;
        u32 pos = 0;
        f32 gain = 0.0f;
        bool active = false;
    };

    void* stream_ = nullptr;  // SDL_AudioStream*
    Voice voices_[16];
};

}  // namespace platform
