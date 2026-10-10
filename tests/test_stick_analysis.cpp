/*
 * DriftCheck - host tests for the stick analysis.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "stick_analysis.hpp"

#include <gtest/gtest.h>

#include <cmath>

using driftcheck::Circularity;
using driftcheck::Point;
using driftcheck::RestTest;
using driftcheck::Verdict;

TEST(StickMath, SquareRootMatchesStd)
{
    for (float value : {1e-6F, 0.01F, 0.25F, 0.5F, 1.0F, 2.0F, 100.0F})
        EXPECT_NEAR(driftcheck::square_root(value), std::sqrt(value), std::sqrt(value) * 1e-5F);
    EXPECT_EQ(driftcheck::square_root(0.0F), 0.0F);
    EXPECT_EQ(driftcheck::square_root(-1.0F), 0.0F);
}

TEST(StickMath, AngleMatchesAtan2EverywhereAround)
{
    for (int step = 0; step < 3600; ++step)
    {
        const double expected = step / 3600.0 * 2.0 * M_PI;
        const Point point{static_cast<float>(std::cos(expected)),
                          static_cast<float>(std::sin(expected))};
        double difference = std::fabs(driftcheck::angle(point) - expected);
        difference = std::fmin(difference, 2.0 * M_PI - difference); // 0 and 2 pi are one angle
        EXPECT_LT(difference, 1e-4) << "step " << step;
    }
}

TEST(StickMath, RestingRawValuesAreNearTheCentre)
{
    const Point rest = driftcheck::normalize(128, 127);
    EXPECT_LT(driftcheck::length(rest) * 100.0F, 1.0F);
    const Point edge = driftcheck::normalize(255, 128);
    EXPECT_NEAR(edge.x, 1.0F, 1e-6F);
    EXPECT_NEAR(driftcheck::normalize(0, 128).x, -1.0F, 1e-6F);
}

TEST(Circularity, BinDirectionsAreUnitVectors)
{
    for (int bin = 0; bin < Circularity::bins; ++bin)
    {
        const Point direction = Circularity::direction(bin);
        EXPECT_NEAR(driftcheck::length(direction), 1.0F, 1e-4F) << "bin " << bin;
    }
}

TEST(Circularity, PerfectCircleHasNoError)
{
    Circularity circle;
    for (int step = 0; step < 720; ++step)
    {
        const double a = step / 720.0 * 2.0 * M_PI;
        circle.add({static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))});
    }
    EXPECT_TRUE(circle.complete());
    EXPECT_LT(circle.error_percent(), 0.1F);
}

TEST(Circularity, ShortStickReportsItsShortfall)
{
    // A stick that only reaches 90 % of the way out in every direction.
    Circularity circle;
    for (int step = 0; step < 720; ++step)
    {
        const double a = step / 720.0 * 2.0 * M_PI;
        circle.add({static_cast<float>(0.9 * std::cos(a)), static_cast<float>(0.9 * std::sin(a))});
    }
    EXPECT_TRUE(circle.complete());
    EXPECT_NEAR(circle.error_percent(), 10.0F, 0.1F);
}

TEST(Circularity, FastTurnFillsTheDirectionsBetweenSamples)
{
    // 20 degrees per sample: four bins wide, as a quick flick at a low sample rate.
    Circularity circle;
    for (int step = 0; step <= 18; ++step)
    {
        const double a = step * 20.0 * M_PI / 180.0;
        circle.add({static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))});
    }
    EXPECT_TRUE(circle.complete());
    EXPECT_LT(circle.error_percent(), 0.5F);
}

TEST(Circularity, JumpAcrossTheCentreIsNotFilled)
{
    Circularity circle;
    circle.add({1.0F, 0.0F});
    circle.add({-1.0F, 0.0F});
    EXPECT_EQ(circle.covered(), 2);
}

TEST(Circularity, CentreSamplesDoNotCount)
{
    Circularity circle;
    circle.add({0.1F, 0.1F});
    EXPECT_EQ(circle.covered(), 0);
    circle.add({1.0F, 0.0F});
    EXPECT_EQ(circle.covered(), 1);
    circle.reset();
    EXPECT_EQ(circle.covered(), 0);
}

TEST(RestTest, FinishesAfterItsDurationEvenWithoutSamples)
{
    RestTest test;
    test.start(1000);
    EXPECT_TRUE(test.running());
    EXPECT_EQ(test.remaining_ms(1500), RestTest::duration_ms - 500);
    test.update(1000 + RestTest::duration_ms);
    EXPECT_TRUE(test.done());
}

TEST(RestTest, MeasuresTheLargestRestingDistance)
{
    RestTest test;
    test.start(0);
    for (std::uint64_t t = 0; t < 1000; t += 10)
        test.add(driftcheck::normalize(128, 128), driftcheck::normalize(140, 128), t);
    const driftcheck::RestResult left = test.left();
    const driftcheck::RestResult right = test.right();
    EXPECT_LT(left.max_percent, 1.0F);
    // 140 is 12.5 steps right of 127.5: 9.8 % of the half range.
    EXPECT_NEAR(right.max_percent, 12.5F / 127.5F * 100.0F, 0.01F);
    EXPECT_NEAR(right.mean_x, 12.5F / 127.5F, 1e-4F);
    EXPECT_FALSE(right.moved);
    EXPECT_EQ(driftcheck::classify(left), Verdict::ok);
    EXPECT_EQ(driftcheck::classify(right), Verdict::slight);
}

TEST(RestTest, TouchingTheStickInvalidatesTheTest)
{
    RestTest test;
    test.start(0);
    test.add(driftcheck::normalize(255, 128), driftcheck::normalize(128, 128), 10);
    EXPECT_TRUE(test.left().moved);
    EXPECT_EQ(driftcheck::classify(test.left()), Verdict::moved);
}

TEST(Deadzone, RoundsUpAndAddsMargin)
{
    EXPECT_EQ(driftcheck::suggested_deadzone_percent({0.0F, 0, 0, false}), 2);
    EXPECT_EQ(driftcheck::suggested_deadzone_percent({3.0F, 0, 0, false}), 5);
    EXPECT_EQ(driftcheck::suggested_deadzone_percent({3.2F, 0, 0, false}), 6);
    EXPECT_EQ(driftcheck::suggested_deadzone_percent({60.0F, 0, 0, false}), 30);
}
