/*
 * DriftCheck - analog stick analysis.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "stick_analysis.hpp"

#include <cstring>

namespace driftcheck
{
namespace
{
constexpr float pi = 3.14159265358979F;
constexpr float two_pi = 2.0F * pi;
constexpr float axis_center = 127.5F;

float absolute(float value) noexcept
{
    return value < 0.0F ? -value : value;
}

// atan on [-1, 1]; polynomial with an error below 1e-5 rad.
float atan_unit(float z) noexcept
{
    const float z2 = z * z;
    return z * (0.9998660F +
                z2 * (-0.3302995F + z2 * (0.1801410F + z2 * (-0.0851330F + z2 * 0.0208351F))));
}

float atan2_approx(float y, float x) noexcept
{
    if (x == 0.0F && y == 0.0F)
        return 0.0F;
    const float ax = absolute(x);
    const float ay = absolute(y);
    float result = ax >= ay ? atan_unit(ay / ax) : pi / 2.0F - atan_unit(ax / ay);
    if (x < 0.0F)
        result = pi - result;
    return y < 0.0F ? -result : result;
}

// sin and cos for the bin directions, by the Taylor series after reducing to [-pi, pi].
float sine(float value) noexcept
{
    while (value > pi)
        value -= two_pi;
    while (value < -pi)
        value += two_pi;
    float term = value;
    float sum = value;
    for (int n = 1; n < 12; ++n)
    {
        term *= -value * value / static_cast<float>((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

float cosine(float value) noexcept
{
    return sine(value + pi / 2.0F);
}
} // namespace

Point normalize(std::uint8_t x, std::uint8_t y) noexcept
{
    return {(static_cast<float>(x) - axis_center) / axis_center,
            (static_cast<float>(y) - axis_center) / axis_center};
}

float square_root(float value) noexcept
{
    if (value <= 0.0F)
        return 0.0F;
    // Halving the exponent gives a first guess within a factor of two; Newton does the rest.
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bits = (bits >> 1U) + 0x1fc00000U;
    float guess = 0.0F;
    std::memcpy(&guess, &bits, sizeof(guess));
    for (int i = 0; i < 4; ++i)
        guess = 0.5F * (guess + value / guess);
    return guess;
}

float length(Point point) noexcept
{
    return square_root(point.x * point.x + point.y * point.y);
}

float angle(Point point) noexcept
{
    const float result = atan2_approx(point.y, point.x);
    return result < 0.0F ? result + two_pi : result;
}

void Circularity::record(int bin, float radius) noexcept
{
    bin = ((bin % bins) + bins) % bins;
    if (radius > radius_[static_cast<std::size_t>(bin)])
        radius_[static_cast<std::size_t>(bin)] = radius;
}

void Circularity::add(Point point) noexcept
{
    const float radius = length(point);
    if (radius < covered_radius)
    {
        has_previous_ = false;
        return;
    }
    const float current = angle(point);
    const float bin_width = two_pi / static_cast<float>(bins);
    if (has_previous_)
    {
        // The short way round from the previous sample; a jump of more than a
        // quarter turn is a different gesture, not a fast one, and is not filled.
        float delta = current - previous_angle_;
        if (delta > pi)
            delta -= two_pi;
        if (delta < -pi)
            delta += two_pi;
        if (absolute(delta) <= pi / 2.0F)
        {
            const int steps = static_cast<int>(absolute(delta) / bin_width);
            for (int step = 1; step <= steps; ++step)
            {
                const float fraction = static_cast<float>(step) / static_cast<float>(steps + 1);
                const float between = previous_angle_ + delta * fraction;
                // The edge between two samples is never further out than either of them.
                const float reach = previous_radius_ < radius ? previous_radius_ : radius;
                record(static_cast<int>((between < 0.0F ? between + two_pi : between) / bin_width),
                       reach);
            }
        }
    }
    record(static_cast<int>(current / bin_width), radius);
    has_previous_ = true;
    previous_angle_ = current;
    previous_radius_ = radius;
}

void Circularity::reset() noexcept
{
    radius_.fill(0.0F);
    has_previous_ = false;
}

float Circularity::radius(int bin) const noexcept
{
    return bin >= 0 && bin < bins ? radius_[static_cast<std::size_t>(bin)] : 0.0F;
}

int Circularity::covered() const noexcept
{
    int count = 0;
    for (const float radius : radius_)
        count += radius >= covered_radius ? 1 : 0;
    return count;
}

bool Circularity::complete() const noexcept
{
    return covered() == bins;
}

float Circularity::error_percent() const noexcept
{
    float sum = 0.0F;
    int count = 0;
    for (const float radius : radius_)
    {
        if (radius < covered_radius)
            continue;
        sum += absolute(radius - 1.0F);
        ++count;
    }
    return count == 0 ? 0.0F : sum / static_cast<float>(count) * 100.0F;
}

Point Circularity::direction(int bin) noexcept
{
    const float middle = (static_cast<float>(bin) + 0.5F) / static_cast<float>(bins) * two_pi;
    return {cosine(middle), sine(middle)};
}

void RestTest::Accumulator::add(Point point) noexcept
{
    const float percent = length(point) * 100.0F;
    if (percent > max_percent)
        max_percent = percent;
    if (percent > moved_percent)
        moved = true;
    sum_x += point.x;
    sum_y += point.y;
}

RestResult RestTest::Accumulator::result(std::uint32_t samples) const noexcept
{
    const double count = samples == 0 ? 1.0 : static_cast<double>(samples);
    return {max_percent, static_cast<float>(sum_x / count), static_cast<float>(sum_y / count),
            moved};
}

void RestTest::start(std::uint64_t now_ms) noexcept
{
    state_ = State::running;
    started_ms_ = now_ms;
    samples_ = 0;
    left_ = {};
    right_ = {};
}

void RestTest::update(std::uint64_t now_ms) noexcept
{
    if (state_ == State::running && now_ms - started_ms_ >= duration_ms)
        state_ = State::done;
}

void RestTest::add(Point left, Point right, std::uint64_t now_ms) noexcept
{
    update(now_ms);
    if (state_ != State::running)
        return;
    left_.add(left);
    right_.add(right);
    ++samples_;
}

bool RestTest::running() const noexcept
{
    return state_ == State::running;
}

bool RestTest::done() const noexcept
{
    return state_ == State::done;
}

std::uint64_t RestTest::remaining_ms(std::uint64_t now_ms) const noexcept
{
    if (state_ != State::running)
        return 0;
    const std::uint64_t elapsed = now_ms - started_ms_;
    return elapsed >= duration_ms ? 0 : duration_ms - elapsed;
}

RestResult RestTest::left() const noexcept
{
    return left_.result(samples_);
}

RestResult RestTest::right() const noexcept
{
    return right_.result(samples_);
}

Verdict classify(const RestResult &result) noexcept
{
    if (result.moved)
        return Verdict::moved;
    if (result.max_percent >= drift_percent)
        return Verdict::drift;
    if (result.max_percent >= slight_drift_percent)
        return Verdict::slight;
    return Verdict::ok;
}

int suggested_deadzone_percent(const RestResult &result) noexcept
{
    int percent = static_cast<int>(result.max_percent);
    if (static_cast<float>(percent) < result.max_percent)
        ++percent;
    percent += 2;
    return percent > 30 ? 30 : percent;
}
} // namespace driftcheck
