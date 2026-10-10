/*
 * DriftCheck - DualSense input through libScePad.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pad.hpp"

#include <array>
#include <cstddef>
#include <cstring>

namespace
{
// scePadRead's sample, 120 bytes. The layout is the PS5 SDL port's PS5_PadData
// (src/joystick/ps5/SDL_ps5joystick.h); the boilerplate's self-update example,
// which ran on hardware, agrees on the size and on where `connected` and the
// timestamp sit.
struct RawTouch
{
    std::uint16_t x;
    std::uint16_t y;
    std::uint8_t id;
    std::uint8_t reserved[3];
};

struct RawSample
{
    std::uint32_t buttons;
    std::uint8_t left_x, left_y, right_x, right_y;
    std::uint8_t l2, r2;
    std::uint16_t padding;
    float orientation[4];
    float motion_a[3];
    float motion_b[3];
    std::uint8_t fingers;
    std::uint8_t reserved0[3];
    std::uint32_t reserved1;
    RawTouch touch[2];
    std::uint8_t connected;
    std::uint64_t timestamp;
    std::uint8_t extension[16];
    std::uint8_t connection_count;
    std::uint8_t reserved2[15];
};
static_assert(sizeof(RawSample) == 120);
static_assert(offsetof(RawSample, left_x) == 4);
static_assert(offsetof(RawSample, l2) == 8);
static_assert(offsetof(RawSample, orientation) == 12);
static_assert(offsetof(RawSample, motion_a) == 28);
static_assert(offsetof(RawSample, motion_b) == 40);
static_assert(offsetof(RawSample, fingers) == 52);
static_assert(offsetof(RawSample, touch) == 60);
static_assert(offsetof(RawSample, connected) == 76);
static_assert(offsetof(RawSample, timestamp) == 80);

std::array<RawSample, 64> samples{};
RawSample last{};
} // namespace

extern "C"
{
    int sceUserServiceInitialize(const void *parameters);
    int sceUserServiceGetInitialUser(int *user);
    int scePadInit(void);
    int scePadOpen(int user, int type, int index, const void *parameters);
    int scePadRead(int handle, RawSample *samples, int count);
}

namespace driftcheck
{
bool Pad::open() noexcept
{
    if (handle_ >= 0)
        return true;
    if (!user_service_ready_)
    {
        (void)sceUserServiceInitialize(nullptr);
        if (scePadInit() < 0)
            return false;
        user_service_ready_ = true;
    }
    int user = 0;
    if (sceUserServiceGetInitialUser(&user) != 0)
        return false;
    handle_ = scePadOpen(user, 0, 0, nullptr);
    return handle_ >= 0;
}

int Pad::read(std::span<PadState> out) noexcept
{
    if (handle_ < 0 || out.empty())
        return 0;
    const int wanted = static_cast<int>(out.size() < samples.size() ? out.size() : samples.size());
    const int count = scePadRead(handle_, samples.data(), wanted);
    if (count <= 0)
        return 0;
    for (int i = 0; i < count; ++i)
    {
        const RawSample &raw = samples[static_cast<std::size_t>(i)];
        PadState &state = out[static_cast<std::size_t>(i)];
        state.buttons = raw.buttons;
        state.left_x = raw.left_x;
        state.left_y = raw.left_y;
        state.right_x = raw.right_x;
        state.right_y = raw.right_y;
        state.l2 = raw.l2;
        state.r2 = raw.r2;
        std::memcpy(state.orientation, raw.orientation, sizeof(state.orientation));
        std::memcpy(state.motion_a, raw.motion_a, sizeof(state.motion_a));
        std::memcpy(state.motion_b, raw.motion_b, sizeof(state.motion_b));
        state.fingers = raw.fingers > 2 ? 2 : raw.fingers;
        for (std::size_t f = 0; f < 2; ++f)
            state.finger[f] = {raw.touch[f].x, raw.touch[f].y, raw.touch[f].id};
        state.connected = raw.connected != 0;
        state.timestamp = raw.timestamp;
    }
    last = samples[static_cast<std::size_t>(count - 1)];
    return count;
}

std::span<const std::uint8_t> Pad::last_raw() const noexcept
{
    return {reinterpret_cast<const std::uint8_t *>(&last), sizeof(last)};
}
} // namespace driftcheck
