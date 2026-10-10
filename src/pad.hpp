/*
 * DriftCheck - DualSense input through libScePad.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <span>

namespace driftcheck
{
// Button bits as libScePad reports them (same values as the PS5 SDL port's
// SDL_ps5joystick.h). Create and the PS button are kept by the system.
namespace button
{
constexpr std::uint32_t l3 = 0x0002;
constexpr std::uint32_t r3 = 0x0004;
constexpr std::uint32_t options = 0x0008;
constexpr std::uint32_t up = 0x0010;
constexpr std::uint32_t right = 0x0020;
constexpr std::uint32_t down = 0x0040;
constexpr std::uint32_t left = 0x0080;
constexpr std::uint32_t l2 = 0x0100;
constexpr std::uint32_t r2 = 0x0200;
constexpr std::uint32_t l1 = 0x0400;
constexpr std::uint32_t r1 = 0x0800;
constexpr std::uint32_t triangle = 0x1000;
constexpr std::uint32_t circle = 0x2000;
constexpr std::uint32_t cross = 0x4000;
constexpr std::uint32_t square = 0x8000;
constexpr std::uint32_t touchpad = 0x100000;
} // namespace button

struct Finger
{
    std::uint16_t x;
    std::uint16_t y;
    std::uint8_t id;
};

struct PadState
{
    std::uint32_t buttons;
    std::uint8_t left_x, left_y, right_x, right_y;
    std::uint8_t l2, r2;
    float orientation[4];
    // The two motion vectors in the order the pad reports them. Which one is the
    // accelerometer is decided at run time: see main.cpp.
    float motion_a[3];
    float motion_b[3];
    std::uint8_t fingers;
    Finger finger[2];
    bool connected;
    std::uint64_t timestamp;
};

class Pad final
{
  public:
    // Opens the controller of the user who started the app. Safe to call again.
    bool open() noexcept;
    [[nodiscard]] bool is_open() const noexcept
    {
        return handle_ >= 0;
    }
    // Every sample since the last call, oldest first; returns how many were written.
    int read(std::span<PadState> out) noexcept;
    // The raw bytes of the last sample read, for the diagnostic log.
    [[nodiscard]] std::span<const std::uint8_t> last_raw() const noexcept;

  private:
    int handle_ = -1;
    bool user_service_ready_ = false;
};
} // namespace driftcheck
