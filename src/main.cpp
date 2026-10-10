/*
 * DriftCheck - a DualSense tester for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shows both sticks with their circularity, a rest test that measures drift and
 * suggests a deadzone, the triggers, every button the system hands to apps, the
 * touchpad and the motion sensors. Drawn on the CPU with the boilerplate renderer.
 *
 * Options starts the rest test; holding Options for two seconds clears the
 * circularity traces. Create and the PS button never reach an app.
 *
 * A diagnostic log goes to /data/driftcheck/driftcheck.log: the raw bytes of a sample
 * and which motion vector behaved like gravity, to confirm the pad layout on a
 * console.
 */

#include "demo_renderer.hpp"
#include "pad.hpp"
#include "stick_analysis.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <fcntl.h>
#include <string_view>

extern "C"
{
    std::uint64_t sceKernelGetProcessTime(void);
    int sceKernelMkdir(const char *path, int mode);
    int sceKernelOpen(const char *path, int flags, int mode);
    std::int64_t sceKernelWrite(int descriptor, const void *buffer, std::size_t length);
}

namespace
{
using driftcheck::PadState;
using driftcheck::Point;
using ps5::demo::Canvas;
using ps5::demo::Color;

// The frame buffer is RGBA in memory: the colour word reads 0xAABBGGRR.
constexpr Color rgb(std::uint32_t r, std::uint32_t g, std::uint32_t b) noexcept
{
    return static_cast<Color>(UINT32_C(0xff000000) | (b << 16U) | (g << 8U) | r);
}

constexpr Color background = rgb(10, 13, 25);
constexpr Color panel = rgb(23, 31, 48);
constexpr Color panel_light = rgb(40, 52, 78);
constexpr Color grey = rgb(110, 120, 140);
constexpr Color white = rgb(240, 240, 240);
constexpr Color cyan = rgb(0, 220, 255);
constexpr Color yellow = rgb(255, 210, 0);
constexpr Color green = rgb(60, 220, 100);
constexpr Color orange = rgb(255, 140, 0);
constexpr Color red = rgb(255, 60, 60);

constexpr std::uint64_t reset_hold_ms = 2000;
// The motion vectors are told apart after this many samples (see note_motion).
constexpr std::uint32_t motion_decision_samples = 500;

driftcheck::Pad pad;
std::array<PadState, 64> batch{};
// Until the first sample, both sticks sit at the centre.
PadState centred() noexcept
{
    PadState state{};
    state.left_x = state.left_y = state.right_x = state.right_y = 128;
    return state;
}
PadState latest = centred();
bool have_sample = false;
bool open_attempted = false;
std::uint64_t last_open_attempt_ms = 0;

driftcheck::Circularity circle_left;
driftcheck::Circularity circle_right;
driftcheck::RestTest rest;
driftcheck::RestResult rest_left{};
driftcheck::RestResult rest_right{};
bool rest_result = false;

std::uint32_t previous_buttons = 0;
std::uint64_t options_down_ms = 0;
bool options_reset_done = false;

// Gravity never stops pulling on the accelerometer, so its average magnitude stays
// near 1 g, while the gyroscope only reads rotation. Whichever vector averages
// higher is the accelerometer; the two pad references disagree on their order.
float average_a = 0.0F;
float average_b = 0.0F;
std::uint32_t motion_samples = 0;
bool motion_logged = false;

std::uint16_t touch_max_x = 1919;
std::uint16_t touch_max_y = 1079;

int log_file = -1;

// Written by the build when BUILD_LABEL is set (docs/PULL_REQUEST_BUILDS.md): a build
// that is not a release says which one it is.
std::array<char, 48> build_label{};

std::uint64_t now_ms() noexcept
{
    return sceKernelGetProcessTime() / 1000U;
}

void log_line(const char *text) noexcept
{
    if (log_file >= 0)
        (void)sceKernelWrite(log_file, text, std::strlen(text));
}

void open_log() noexcept
{
    (void)sceKernelMkdir("/data/driftcheck", 0777);
    log_file = sceKernelOpen("/data/driftcheck/driftcheck.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    log_line("DriftCheck started\n");
}

// The renderer's font has capitals, digits and a few signs.
void text(Canvas &canvas, unsigned x, unsigned y, const char *value, unsigned scale,
          Color color) noexcept
{
    std::array<char, 96> upper{};
    std::size_t length = 0;
    for (; value[length] != '\0' && length + 1 < upper.size(); ++length)
    {
        const char c = value[length];
        upper[length] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    }
    canvas.text(x, y, std::string_view{upper.data(), length}, scale, color);
}

// Fixed-point text without relying on the runtime's floating-point printf.
void fixed(char *out, std::size_t size, float value, int decimals) noexcept
{
    const bool negative = value < 0.0F;
    if (negative)
        value = -value;
    const long scale = decimals == 1 ? 10 : 100;
    const long units = static_cast<long>(value * static_cast<float>(scale) + 0.5F);
    (void)std::snprintf(out, size, "%s%ld.%0*ld", negative && units != 0 ? "-" : "", units / scale,
                        decimals, units % scale);
}

float magnitude(const float vector[3]) noexcept
{
    return driftcheck::square_root(vector[0] * vector[0] + vector[1] * vector[1] +
                                   vector[2] * vector[2]);
}

void dot(Canvas &canvas, int x, int y, unsigned size, Color color) noexcept
{
    if (x < 0 || y < 0)
        return;
    canvas.rectangle(static_cast<unsigned>(x), static_cast<unsigned>(y), size, size, color);
}

// Midpoint circle outline.
void ring(Canvas &canvas, int cx, int cy, int radius, Color color) noexcept
{
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y)
    {
        const std::array<std::array<int, 2>, 8> points{
            {{x, y}, {y, x}, {-y, x}, {-x, y}, {-x, -y}, {-y, -x}, {y, -x}, {x, -y}}};
        for (const auto &point : points)
            dot(canvas, cx + point[0] - 1, cy + point[1] - 1, 3, color);
        ++y;
        if (error < 0)
        {
            error += 2 * y + 1;
        }
        else
        {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
}

void note_motion(const PadState &state) noexcept
{
    const float a = magnitude(state.motion_a);
    const float b = magnitude(state.motion_b);
    // A running mean that settles over a few hundred samples.
    average_a += (a - average_a) / 128.0F;
    average_b += (b - average_b) / 128.0F;
    ++motion_samples;
    if (motion_samples >= motion_decision_samples && !motion_logged)
    {
        motion_logged = true;
        std::array<char, 160> line{};
        std::array<char, 16> ta{};
        std::array<char, 16> tb{};
        fixed(ta.data(), ta.size(), average_a, 2);
        fixed(tb.data(), tb.size(), average_b, 2);
        (void)std::snprintf(line.data(), line.size(),
                            "motion: mean |a| %s, mean |b| %s -> accelerometer is %s\n", ta.data(),
                            tb.data(), average_a >= average_b ? "a" : "b");
        log_line(line.data());
        log_line("raw sample:");
        for (const std::uint8_t byte : pad.last_raw())
        {
            std::array<char, 4> hex{};
            (void)std::snprintf(hex.data(), hex.size(), " %02x", byte);
            log_line(hex.data());
        }
        log_line("\n");
    }
}

void handle_options(std::uint32_t buttons, std::uint64_t now) noexcept
{
    const bool down = (buttons & driftcheck::button::options) != 0;
    const bool was_down = (previous_buttons & driftcheck::button::options) != 0;
    if (down && !was_down)
    {
        options_down_ms = now;
        options_reset_done = false;
    }
    if (down && !options_reset_done && now - options_down_ms >= reset_hold_ms)
    {
        circle_left.reset();
        circle_right.reset();
        options_reset_done = true;
    }
    // The test starts on release, once the hand has left the button.
    if (!down && was_down && !options_reset_done)
    {
        rest.start(now);
        rest_result = false;
    }
}

void read_pad(std::uint64_t now) noexcept
{
    if (!pad.is_open() && (!open_attempted || now - last_open_attempt_ms >= 1000))
    {
        open_attempted = true;
        last_open_attempt_ms = now;
        if (pad.open())
            log_line("controller opened\n");
    }
    const int count = pad.read(batch);
    for (int i = 0; i < count; ++i)
    {
        const PadState &state = batch[static_cast<std::size_t>(i)];
        const Point left = driftcheck::normalize(state.left_x, state.left_y);
        const Point right = driftcheck::normalize(state.right_x, state.right_y);
        if (rest.running())
        {
            rest.add(left, right, now);
        }
        else
        {
            circle_left.add(left);
            circle_right.add(right);
        }
        handle_options(state.buttons, now);
        previous_buttons = state.buttons;
        note_motion(state);
        for (std::size_t f = 0; f < state.fingers; ++f)
        {
            if (state.finger[f].x > touch_max_x)
                touch_max_x = state.finger[f].x;
            if (state.finger[f].y > touch_max_y)
                touch_max_y = state.finger[f].y;
        }
        latest = state;
        have_sample = true;
    }
    rest.update(now);
    if (rest.done() && !rest_result)
    {
        rest_left = rest.left();
        rest_right = rest.right();
        rest_result = true;
        std::array<char, 96> line{};
        std::array<char, 16> l{};
        std::array<char, 16> r{};
        fixed(l.data(), l.size(), rest_left.max_percent, 1);
        fixed(r.data(), r.size(), rest_right.max_percent, 1);
        (void)std::snprintf(line.data(), line.size(), "rest test: left %s%%, right %s%%\n",
                            l.data(), r.data());
        log_line(line.data());
    }
}

const char *verdict_text(driftcheck::Verdict verdict) noexcept
{
    switch (verdict)
    {
    case driftcheck::Verdict::ok:
        return "OK";
    case driftcheck::Verdict::slight:
        return "SLIGHT DRIFT";
    case driftcheck::Verdict::drift:
        return "DRIFT";
    case driftcheck::Verdict::moved:
        return "TOUCHED - REDO";
    }
    return "";
}

Color verdict_color(driftcheck::Verdict verdict) noexcept
{
    switch (verdict)
    {
    case driftcheck::Verdict::ok:
        return green;
    case driftcheck::Verdict::slight:
        return orange;
    case driftcheck::Verdict::drift:
        return red;
    case driftcheck::Verdict::moved:
        return grey;
    }
    return white;
}

void draw_stick(Canvas &canvas, unsigned x, unsigned y, const char *name, std::uint8_t raw_x,
                std::uint8_t raw_y, const driftcheck::Circularity &circle,
                const driftcheck::RestResult &result) noexcept
{
    constexpr unsigned size = 420;
    constexpr int radius = 180;
    const int cx = static_cast<int>(x + size / 2);
    const int cy = static_cast<int>(y + size / 2);
    canvas.rectangle(x, y, size, size, panel);
    canvas.rectangle(x + 20, static_cast<unsigned>(cy), size - 40, 1, panel_light);
    canvas.rectangle(static_cast<unsigned>(cx), y + 20, 1, size - 40, panel_light);
    ring(canvas, cx, cy, radius, grey);
    if (rest_result && !result.moved)
    {
        const int deadzone = driftcheck::suggested_deadzone_percent(result);
        ring(canvas, cx, cy, radius * deadzone / 100, orange);
    }
    for (int bin = 0; bin < driftcheck::Circularity::bins; ++bin)
    {
        const float reach = circle.radius(bin);
        if (reach < driftcheck::Circularity::covered_radius)
            continue;
        const Point direction = driftcheck::Circularity::direction(bin);
        const float shown = reach > 1.2F ? 1.2F : reach;
        const float off = reach > 1.0F ? reach - 1.0F : 1.0F - reach;
        dot(canvas, cx + static_cast<int>(direction.x * shown * radius) - 3,
            cy + static_cast<int>(direction.y * shown * radius) - 3, 7,
            off < 0.05F ? green : yellow);
    }
    const Point now = driftcheck::normalize(raw_x, raw_y);
    canvas.circle(static_cast<unsigned>(cx + static_cast<int>(now.x * radius)),
                  static_cast<unsigned>(cy + static_cast<int>(now.y * radius)), 12, cyan);

    std::array<char, 48> line{};
    unsigned ty = y + size + 16;
    text(canvas, x, ty, name, 4, white);
    ty += 44;
    (void)std::snprintf(line.data(), line.size(), "X %3u  Y %3u", raw_x, raw_y);
    text(canvas, x, ty, line.data(), 3, white);
    ty += 32;
    std::array<char, 16> number{};
    fixed(number.data(), number.size(), driftcheck::length(now) * 100.0F, 1);
    (void)std::snprintf(line.data(), line.size(), "DISTANCE %s%%", number.data());
    text(canvas, x, ty, line.data(), 3, white);
    ty += 32;
    if (circle.complete())
    {
        fixed(number.data(), number.size(), circle.error_percent(), 1);
        (void)std::snprintf(line.data(), line.size(), "CIRCULARITY %s%%", number.data());
        text(canvas, x, ty, line.data(), 3, white);
    }
    else
    {
        (void)std::snprintf(line.data(), line.size(), "ROTATE FULLY %d/%d", circle.covered(),
                            driftcheck::Circularity::bins);
        text(canvas, x, ty, line.data(), 3, grey);
    }
    ty += 32;
    if (rest_result)
    {
        const driftcheck::Verdict verdict = driftcheck::classify(result);
        fixed(number.data(), number.size(), result.max_percent, 1);
        (void)std::snprintf(line.data(), line.size(), "REST %s%% %s", number.data(),
                            verdict_text(verdict));
        text(canvas, x, ty, line.data(), 3, verdict_color(verdict));
        ty += 32;
        if (!result.moved)
        {
            (void)std::snprintf(line.data(), line.size(), "DEADZONE %d%%",
                                driftcheck::suggested_deadzone_percent(result));
            text(canvas, x, ty, line.data(), 3, orange);
        }
    }
    else
    {
        text(canvas, x, ty, "REST TEST: PRESS OPTIONS", 3, grey);
    }
}

void draw_trigger(Canvas &canvas, unsigned y, const char *name, std::uint8_t value) noexcept
{
    constexpr unsigned x = 1060;
    constexpr unsigned width = 600;
    text(canvas, x, y + 8, name, 4, white);
    canvas.rectangle(x + 80, y, width, 40, panel);
    canvas.rectangle(x + 80, y, width * value / 255U, 40, value == 255 ? green : cyan);
    std::array<char, 8> number{};
    (void)std::snprintf(number.data(), number.size(), "%3u", value);
    text(canvas, x + 80 + width + 20, y + 8, number.data(), 4, white);
}

void draw_buttons(Canvas &canvas, std::uint32_t buttons) noexcept
{
    struct Cell
    {
        const char *name;
        std::uint32_t bit;
    };
    namespace b = driftcheck::button;
    constexpr std::array<Cell, 16> cells{{{"CROSS", b::cross},
                                          {"CIRCLE", b::circle},
                                          {"SQUARE", b::square},
                                          {"TRIANGLE", b::triangle},
                                          {"L1", b::l1},
                                          {"R1", b::r1},
                                          {"L2", b::l2},
                                          {"R2", b::r2},
                                          {"L3", b::l3},
                                          {"R3", b::r3},
                                          {"OPTIONS", b::options},
                                          {"TOUCHPAD", b::touchpad},
                                          {"UP", b::up},
                                          {"DOWN", b::down},
                                          {"LEFT", b::left},
                                          {"RIGHT", b::right}}};
    for (std::size_t i = 0; i < cells.size(); ++i)
    {
        const unsigned x = 1060 + static_cast<unsigned>(i % 4) * 200;
        const unsigned y = 300 + static_cast<unsigned>(i / 4) * 66;
        const bool pressed = (buttons & cells[i].bit) != 0;
        canvas.rectangle(x, y, 190, 56, pressed ? yellow : panel);
        text(canvas, x + 14, y + 18, cells[i].name, 3, pressed ? background : white);
    }
}

void draw_touchpad(Canvas &canvas, const PadState &state) noexcept
{
    constexpr unsigned x = 1060;
    constexpr unsigned y = 600;
    constexpr unsigned width = 480;
    constexpr unsigned height = 270;
    text(canvas, x, y, "TOUCHPAD", 3, white);
    canvas.rectangle(x, y + 30, width, height, panel);
    std::array<char, 48> line{};
    for (std::size_t f = 0; f < state.fingers; ++f)
    {
        const driftcheck::Finger &finger = state.finger[f];
        const unsigned fx = x + finger.x * (width - 1) / touch_max_x;
        const unsigned fy = y + 30 + finger.y * (height - 1) / touch_max_y;
        canvas.circle(fx, fy, 14, f == 0 ? cyan : yellow);
        (void)std::snprintf(line.data(), line.size(), "%u: %4u %4u", static_cast<unsigned>(f + 1),
                            finger.x, finger.y);
        text(canvas, x, y + 40 + height + static_cast<unsigned>(f) * 30, line.data(), 3,
             f == 0 ? cyan : yellow);
    }
}

void draw_vector(Canvas &canvas, unsigned y, const char *name, const float vector[3]) noexcept
{
    constexpr unsigned x = 1580;
    text(canvas, x, y, name, 3, white);
    constexpr std::array<const char *, 3> axes{"X", "Y", "Z"};
    for (std::size_t i = 0; i < 3; ++i)
    {
        std::array<char, 16> number{};
        std::array<char, 24> line{};
        fixed(number.data(), number.size(), vector[i], 2);
        (void)std::snprintf(line.data(), line.size(), "%s %s", axes[i], number.data());
        text(canvas, x, y + 32 + static_cast<unsigned>(i) * 30, line.data(), 3, grey);
    }
}

void draw_motion(Canvas &canvas, const PadState &state) noexcept
{
    const bool decided = motion_samples >= motion_decision_samples;
    const bool a_is_accel = average_a >= average_b;
    draw_vector(canvas, 600, decided ? (a_is_accel ? "ACCEL" : "GYRO") : "MOTION A",
                state.motion_a);
    draw_vector(canvas, 760, decided ? (a_is_accel ? "GYRO" : "ACCEL") : "MOTION B",
                state.motion_b);
}

void draw_scene(Canvas &canvas) noexcept
{
    const std::uint64_t now = now_ms();
    read_pad(now);

    canvas.clear(background);
    text(canvas, 80, 40, "DRIFTCHECK", 7, yellow);
    text(canvas, 80, 104, "DUALSENSE TESTER", 3, grey);
    const bool connected = have_sample && latest.connected;
    text(canvas, 1060, 60, connected ? "CONTROLLER CONNECTED" : "WAITING FOR CONTROLLER", 4,
         connected ? green : orange);

    draw_stick(canvas, 80, 170, "LEFT STICK", latest.left_x, latest.left_y, circle_left, rest_left);
    draw_stick(canvas, 560, 170, "RIGHT STICK", latest.right_x, latest.right_y, circle_right,
               rest_right);
    draw_trigger(canvas, 170, "L2", latest.l2);
    draw_trigger(canvas, 230, "R2", latest.r2);
    draw_buttons(canvas, latest.buttons);
    draw_touchpad(canvas, latest);
    draw_motion(canvas, latest);

    text(canvas, 80, 1020, "OPTIONS: REST TEST     HOLD OPTIONS 2 S: CLEAR CIRCLES", 3, grey);
    if (build_label[0] != '\0')
        text(canvas, 1060, 1020, build_label.data(), 3, yellow);

    if (rest.running())
    {
        canvas.rectangle(80, 300, 900, 170, panel_light);
        text(canvas, 110, 330, "LET GO OF BOTH STICKS", 5, yellow);
        std::array<char, 16> seconds{};
        fixed(seconds.data(), seconds.size(), static_cast<float>(rest.remaining_ms(now)) / 1000.0F,
              1);
        text(canvas, 110, 400, seconds.data(), 6, white);
    }
}
} // namespace

int main()
{
    open_log();
    ps5::demo::read_asset_text("/app0/build-label.txt", std::span{build_label}, "");
    ps5::demo::run_frames(draw_scene, "DriftCheck ready");
}
