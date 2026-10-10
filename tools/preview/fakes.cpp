/*
 * DriftCheck - stand-ins for the console calls, for the host preview only.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One frame is one pad sample, 33 ms apart. The script the fake controller follows:
 *   frames   0-149  left stick turns full circles, right stick turns at 93 % reach
 *   frames 160-161  Options pressed and released: the rest test starts
 *   frames 162-259  sticks released; the right one rests off-centre (slight drift)
 *   frames 260-     Cross, R1 and Up held, L2 half, R2 full, two fingers on the touchpad
 */
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
std::uint64_t frame = 0;
std::uint64_t clock_us = 0;

void put8(unsigned char *s, int offset, std::uint8_t v)
{
    s[offset] = v;
}
void put16(unsigned char *s, int offset, std::uint16_t v)
{
    std::memcpy(s + offset, &v, 2);
}
void put32(unsigned char *s, int offset, std::uint32_t v)
{
    std::memcpy(s + offset, &v, 4);
}
void putf(unsigned char *s, int offset, float v)
{
    std::memcpy(s + offset, &v, 4);
}
std::uint8_t axis(double v)
{
    return static_cast<std::uint8_t>(std::lround(127.5 + 127.5 * v));
}
} // namespace

extern "C"
{
    // Called once per drawn frame: each frame is 33 ms later than the last.
    std::uint64_t sceKernelGetProcessTime(void)
    {
        clock_us += 33000U;
        return clock_us;
    }
    int sceKernelMkdir(const char *, int)
    {
        return 0;
    }
    int sceKernelOpen(const char *, int, int)
    {
        return -1;
    }
    std::int64_t sceKernelWrite(int, const void *, std::size_t)
    {
        return 0;
    }
    int sceUserServiceInitialize(const void *)
    {
        return 0;
    }
    int sceUserServiceGetInitialUser(int *user)
    {
        *user = 1;
        return 0;
    }
    int scePadInit(void)
    {
        return 0;
    }
    int scePadOpen(int, int, int, const void *)
    {
        return 1;
    }

    int scePadRead(int, void *out, int count)
    {
        if (count < 1)
            return 0;
        unsigned char *s = static_cast<unsigned char *>(out);
        std::memset(s, 0, 120);
        const double angle = static_cast<double>(frame) * 0.05;
        std::uint32_t buttons = 0;
        double lx = 0, ly = 0, rx = 0, ry = 0;
        std::uint8_t l2 = 0, r2 = 0, fingers = 0;
        if (frame < 150)
        {
            lx = std::cos(angle), ly = std::sin(angle);
            rx = 0.93 * std::cos(-angle), ry = 0.93 * std::sin(-angle);
        }
        else if (frame == 160)
            buttons = 0x0008; // Options down; released on the next frame
        else if (frame >= 162 && frame < 260)
            rx = 11.0 / 127.5, ry = 4.0 / 127.5;
        else if (frame >= 260)
        {
            buttons = 0x4000 | 0x0800 | 0x0010 | 0x0200; // Cross, R1, Up, R2
            lx = 0.55, ly = -0.45;
            rx = 11.0 / 127.5, ry = 4.0 / 127.5;
            l2 = 128, r2 = 255, fingers = 2;
        }
        put32(s, 0, buttons);
        put8(s, 4, axis(lx));
        put8(s, 5, axis(ly));
        put8(s, 6, axis(rx));
        put8(s, 7, axis(ry));
        put8(s, 8, l2);
        put8(s, 9, r2);
        putf(s, 28, 0.01F), putf(s, 32, -0.02F), putf(s, 36, 0.0F); // reads like a gyro at rest
        putf(s, 40, 0.02F), putf(s, 44, 0.98F), putf(s, 48, 0.12F); // reads like gravity
        put8(s, 52, fingers);
        put16(s, 60, 420), put16(s, 62, 300), put8(s, 64, 0);
        put16(s, 68, 1500), put16(s, 70, 760), put8(s, 72, 1);
        put8(s, 76, 1);
        ++frame;
        return 1;
    }
}
