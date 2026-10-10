/*
 * DriftCheck - analog stick analysis.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure math with no platform calls, so the host unit tests can run it. The runtime
 * shim does not promise libm, so square root and atan2 are computed here.
 */
#pragma once

#include <array>
#include <cstdint>

namespace driftcheck
{
// A stick position in [-1, 1] on both axes; y grows downwards, as the raw value does.
struct Point
{
    float x;
    float y;
};

// The raw axes run 0..255 with the rest position between 127 and 128.
[[nodiscard]] Point normalize(std::uint8_t x, std::uint8_t y) noexcept;
[[nodiscard]] float square_root(float value) noexcept;
[[nodiscard]] float length(Point point) noexcept;
// Angle in [0, 2 pi), measured from +x towards +y.
[[nodiscard]] float angle(Point point) noexcept;

// The largest radius reached in each direction while the player rotates the stick.
class Circularity final
{
  public:
    static constexpr int bins = 72;
    // A direction counts once the stick went at least this far out in it.
    static constexpr float covered_radius = 0.5F;

    void add(Point point) noexcept;
    void reset() noexcept;
    [[nodiscard]] float radius(int bin) const noexcept;
    [[nodiscard]] int covered() const noexcept;
    [[nodiscard]] bool complete() const noexcept;
    // Mean distance from the unit circle over every direction, in percent. Only
    // meaningful once complete(): a half-done rotation reports the half it saw.
    [[nodiscard]] float error_percent() const noexcept;
    // Unit vector at the centre of a bin, for drawing.
    [[nodiscard]] static Point direction(int bin) noexcept;

  private:
    void record(int bin, float radius) noexcept;

    std::array<float, bins> radius_{};
    // The previous sample out at the edge, so a fast turn that jumps over a few
    // directions between two samples still fills them.
    bool has_previous_ = false;
    float previous_angle_ = 0.0F;
    float previous_radius_ = 0.0F;
};

// How far a released stick sits from the centre over a few seconds.
struct RestResult
{
    float max_percent; // largest distance from the centre seen
    float mean_x;      // average offset, normalised
    float mean_y;
    bool moved; // the stick was touched during the test
};

class RestTest final
{
  public:
    static constexpr std::uint64_t duration_ms = 3000;
    // Above this the stick was being held, not resting: the test is invalid.
    static constexpr float moved_percent = 25.0F;

    void start(std::uint64_t now_ms) noexcept;
    void add(Point left, Point right, std::uint64_t now_ms) noexcept;
    // Ends the test once its time is up, even if no sample arrives.
    void update(std::uint64_t now_ms) noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool done() const noexcept;
    // Milliseconds left while running.
    [[nodiscard]] std::uint64_t remaining_ms(std::uint64_t now_ms) const noexcept;
    [[nodiscard]] RestResult left() const noexcept;
    [[nodiscard]] RestResult right() const noexcept;

  private:
    struct Accumulator
    {
        float max_percent;
        double sum_x;
        double sum_y;
        bool moved;
        void add(Point point) noexcept;
        [[nodiscard]] RestResult result(std::uint32_t samples) const noexcept;
    };

    enum class State
    {
        idle,
        running,
        done
    };
    State state_ = State::idle;
    std::uint64_t started_ms_ = 0;
    std::uint32_t samples_ = 0;
    Accumulator left_{};
    Accumulator right_{};
};

enum class Verdict
{
    ok,
    slight,
    drift,
    moved
};

// The thresholds are a starting heuristic, not a measured standard: a healthy
// DualSense should rest within a few percent of the centre.
constexpr float slight_drift_percent = 5.0F;
constexpr float drift_percent = 10.0F;

[[nodiscard]] Verdict classify(const RestResult &result) noexcept;
// The resting distance rounded up, plus a two-point margin, capped at 30 %.
[[nodiscard]] int suggested_deadzone_percent(const RestResult &result) noexcept;
} // namespace driftcheck
