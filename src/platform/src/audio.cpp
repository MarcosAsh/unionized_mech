#include "platform/audio.h"

#include <SDL3/SDL.h>

#include <cmath>

namespace platform {

namespace {

constexpr u32 RATE = 48000;
constexpr u32 MAX_SAMPLES = RATE / 2;  // half a second, the longest sting
constexpr u32 SOUND_COUNT = static_cast<u32>(Sound::Count);

f32 bank[SOUND_COUNT][MAX_SAMPLES];
u32 bank_len[SOUND_COUNT];
bool bank_ready = false;

// Deterministic noise; audio is cosmetic but there is no reason to vary.
f32 noise(u32& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<f32>(state & 0xFFFF) * (2.0f / 65535.0f) - 1.0f;
}

u32 synth(Sound sound, f32 seconds, f32 (*sample)(f32, u32&)) {
    u32 len = static_cast<u32>(seconds * static_cast<f32>(RATE));
    if (len > MAX_SAMPLES) {
        len = MAX_SAMPLES;
    }
    u32 rng = 0x2F6E2B1u;
    f32* out = bank[static_cast<u32>(sound)];
    for (u32 i = 0; i < len; ++i) {
        out[i] = sample(static_cast<f32>(i) / static_cast<f32>(RATE), rng);
    }
    // A short fade-out keeps the tail from clicking.
    const u32 fade = len < 512 ? len : 512;
    for (u32 i = 0; i < fade; ++i) {
        out[len - 1 - i] *= static_cast<f32>(i) / static_cast<f32>(fade);
    }
    return len;
}

constexpr f32 TAU = 6.2831853f;

void synth_bank() {
    if (bank_ready) {
        return;
    }
    bank_ready = true;
    bank_len[static_cast<u32>(Sound::Fire)] = synth(Sound::Fire, 0.09f, [](f32 t, u32& rng) {
        const f32 crack = noise(rng) * std::exp(-t * 55.0f) * 0.55f;
        const f32 punch = std::sin(TAU * (150.0f - 300.0f * t) * t) * std::exp(-t * 32.0f) * 0.8f;
        return crack + punch;
    });
    bank_len[static_cast<u32>(Sound::MechFire)] =
        synth(Sound::MechFire, 0.16f, [](f32 t, u32& rng) {
            const f32 thump = std::sin(TAU * (85.0f - 120.0f * t) * t) * std::exp(-t * 18.0f);
            return thump * 0.9f + noise(rng) * std::exp(-t * 32.0f) * 0.45f;
        });
    bank_len[static_cast<u32>(Sound::Union)] = synth(Sound::Union, 0.3f, [](f32 t, u32&) {
        const f32 sweep = std::sin(TAU * (220.0f + 900.0f * t) * t) * 0.35f;
        return sweep * std::exp(-t * 5.0f) + std::sin(TAU * 110.0f * t) * std::exp(-t * 8.0f) * 0.3f;
    });
    bank_len[static_cast<u32>(Sound::Boom)] = synth(Sound::Boom, 0.5f, [](f32 t, u32& rng) {
        return noise(rng) * std::exp(-t * 7.0f) * 0.75f +
               std::sin(TAU * 55.0f * t) * std::exp(-t * 5.0f) * 0.7f;
    });
    bank_len[static_cast<u32>(Sound::Hit)] = synth(Sound::Hit, 0.07f, [](f32 t, u32&) {
        return std::sin(TAU * 1320.0f * t) * std::exp(-t * 70.0f) * 0.6f +
               std::sin(TAU * 1980.0f * t) * std::exp(-t * 90.0f) * 0.25f;
    });
    bank_len[static_cast<u32>(Sound::Hurt)] = synth(Sound::Hurt, 0.14f, [](f32 t, u32& rng) {
        return std::sin(TAU * 95.0f * t) * std::exp(-t * 26.0f) * 0.9f +
               noise(rng) * std::exp(-t * 45.0f) * 0.2f;
    });
    bank_len[static_cast<u32>(Sound::Death)] = synth(Sound::Death, 0.4f, [](f32 t, u32&) {
        const f32 freq = 380.0f - 620.0f * t;
        return std::sin(TAU * freq * t) * std::exp(-t * 7.0f) * 0.8f;
    });
    bank_len[static_cast<u32>(Sound::Win)] = synth(Sound::Win, 0.45f, [](f32 t, u32&) {
        const f32 notes[3] = {392.0f, 523.25f, 659.25f};
        const u32 step = t < 0.15f ? 0u : (t < 0.3f ? 1u : 2u);
        const f32 local = t - static_cast<f32>(step) * 0.15f;
        return std::sin(TAU * notes[step] * t) * std::exp(-local * 14.0f) * 0.5f;
    });
    bank_len[static_cast<u32>(Sound::Lose)] = synth(Sound::Lose, 0.45f, [](f32 t, u32&) {
        const f32 notes[3] = {392.0f, 329.63f, 261.63f};
        const u32 step = t < 0.15f ? 0u : (t < 0.3f ? 1u : 2u);
        const f32 local = t - static_cast<f32>(step) * 0.15f;
        return std::sin(TAU * notes[step] * t) * std::exp(-local * 12.0f) * 0.5f;
    });
}

}  // namespace

core::Result<Audio, const char*> Audio::open() {
    using OpenResult = core::Result<Audio, const char*>;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return OpenResult::err("audio subsystem init failed");
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 1;
    spec.freq = static_cast<int>(RATE);
    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr) {
        return OpenResult::err("no audio output device");
    }
    synth_bank();
    SDL_ResumeAudioStreamDevice(stream);
    Audio audio;
    audio.stream_ = stream;
    return OpenResult::ok(static_cast<Audio&&>(audio));
}

Audio::~Audio() {
    if (stream_ != nullptr) {
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(stream_));
        SDL_QuitSubSystem(SDL_INIT_AUDIO);  // pairs the init in open()
    }
}

Audio::Audio(Audio&& other) noexcept { *this = static_cast<Audio&&>(other); }

Audio& Audio::operator=(Audio&& other) noexcept {
    if (this != &other) {
        stream_ = other.stream_;
        for (u32 i = 0; i < 16; ++i) {
            voices_[i] = other.voices_[i];
        }
        other.stream_ = nullptr;
    }
    return *this;
}

void Audio::play(Sound sound, f32 gain) {
    if (stream_ == nullptr || gain <= 0.0f) {
        return;
    }
    for (Voice& voice : voices_) {
        if (voice.active) {
            continue;
        }
        voice.sound = static_cast<u32>(sound);
        voice.pos = 0;
        voice.gain = gain > 1.0f ? 1.0f : gain;
        voice.active = true;
        return;
    }
}

void Audio::update() {
    if (stream_ == nullptr) {
        return;
    }
    constexpr u32 TARGET_QUEUED = RATE / 10;  // 100ms of headroom
    constexpr u32 CHUNK = 512;
    SDL_AudioStream* stream = static_cast<SDL_AudioStream*>(stream_);
    while (SDL_GetAudioStreamQueued(stream) <
           static_cast<int>(TARGET_QUEUED * sizeof(f32))) {
        f32 mix[CHUNK] = {};
        for (Voice& voice : voices_) {
            if (!voice.active) {
                continue;
            }
            const f32* data = bank[voice.sound];
            const u32 len = bank_len[voice.sound];
            u32 count = len - voice.pos;
            if (count > CHUNK) {
                count = CHUNK;
            }
            for (u32 i = 0; i < count; ++i) {
                mix[i] += data[voice.pos + i] * voice.gain;
            }
            voice.pos += count;
            if (voice.pos >= len) {
                voice.active = false;
            }
        }
        for (u32 i = 0; i < CHUNK; ++i) {
            if (mix[i] > 1.0f) {
                mix[i] = 1.0f;
            }
            if (mix[i] < -1.0f) {
                mix[i] = -1.0f;
            }
        }
        SDL_PutAudioStreamData(stream, mix, CHUNK * sizeof(f32));
    }
}

}  // namespace platform
