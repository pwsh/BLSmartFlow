// Unity tests for the Arduino-free fan curve module (src/blflow/curve.h).
// Run with: pio test -e native

#include <unity.h>
#include <math.h>
#include "curve.h"

using namespace blsf;

static FanCurve makeCurve(const CurvePoint* pts, uint8_t n)
{
    FanCurve c{};
    c.count = n;
    for (uint8_t i = 0; i < n; i++) c.pts[i] = pts[i];
    return c;
}

void setUp(void) {}
void tearDown(void) {}

// --- interpolation ---------------------------------------------------------

static void test_below_first_point(void)
{
    FanCurve c; curveDefaults(c);
    TEST_ASSERT_TRUE(curveValidate(c));
    // Below the first point clamps to the first speed.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, curveInterpolate(c, -40.0f));
}

static void test_above_last_point(void)
{
    FanCurve c; curveDefaults(c);
    TEST_ASSERT_TRUE(curveValidate(c));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, curveInterpolate(c, 500.0f));
    TEST_ASSERT_EQUAL_FLOAT(100.0f, curveInterpolate(c, 350.0f));
}

static void test_exact_points(void)
{
    FanCurve c; curveDefaults(c);
    TEST_ASSERT_TRUE(curveValidate(c));
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  curveInterpolate(c, 50.0f));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, curveInterpolate(c, 180.0f));
    TEST_ASSERT_EQUAL_FLOAT(80.0f, curveInterpolate(c, 245.0f));
}

static void test_midpoint(void)
{
    const CurvePoint pts[] = { {0.0f, 0}, {100.0f, 100} };
    FanCurve c = makeCurve(pts, 2);
    TEST_ASSERT_TRUE(curveValidate(c));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, curveInterpolate(c, 50.0f));
    TEST_ASSERT_EQUAL_FLOAT(25.0f, curveInterpolate(c, 25.0f));
    // Segment midpoint of the default curve: halfway 180->245 is 50->80.
    FanCurve d; curveDefaults(d); curveValidate(d);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 65.0f, curveInterpolate(d, 212.5f));
}

// --- validation ------------------------------------------------------------

static void test_unsorted_input_is_sorted(void)
{
    const CurvePoint pts[] = { {200.0f, 80}, {0.0f, 0}, {100.0f, 40} };
    FanCurve c = makeCurve(pts, 3);
    TEST_ASSERT_TRUE(curveValidate(c));
    TEST_ASSERT_EQUAL_UINT8(3, c.count);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,   c.pts[0].temp);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, c.pts[1].temp);
    TEST_ASSERT_EQUAL_FLOAT(200.0f, c.pts[2].temp);
    TEST_ASSERT_EQUAL_FLOAT(40.0f,  curveInterpolate(c, 100.0f));
}

static void test_duplicate_temps_deduped(void)
{
    // Two points at 100 C: the later entry wins.
    const CurvePoint pts[] = { {0.0f, 0}, {100.0f, 10}, {100.0f, 90}, {200.0f, 100} };
    FanCurve c = makeCurve(pts, 4);
    TEST_ASSERT_TRUE(curveValidate(c));
    TEST_ASSERT_EQUAL_UINT8(3, c.count);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, curveInterpolate(c, 100.0f));
}

static void test_fewer_than_two_points_invalid(void)
{
    const CurvePoint one[] = { {100.0f, 50} };
    FanCurve c = makeCurve(one, 1);
    TEST_ASSERT_FALSE(curveValidate(c));

    FanCurve empty{};
    empty.count = 0;
    TEST_ASSERT_FALSE(curveValidate(empty));
    // An empty curve must not be undefined behaviour - it reads as 0 %.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, curveInterpolate(empty, 250.0f));

    // Duplicates collapsing down to one point are also invalid.
    const CurvePoint dup[] = { {50.0f, 10}, {50.0f, 20} };
    FanCurve d = makeCurve(dup, 2);
    TEST_ASSERT_FALSE(curveValidate(d));
}

static void test_speed_clamped(void)
{
    CurvePoint pts[] = { {0.0f, 0}, {100.0f, 200} };  // 200 % is out of range
    FanCurve c = makeCurve(pts, 2);
    TEST_ASSERT_TRUE(curveValidate(c));
    TEST_ASSERT_EQUAL_UINT8(100, c.pts[1].speed);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, curveInterpolate(c, 100.0f));
}

static void test_temp_clamped(void)
{
    const CurvePoint pts[] = { {-50.0f, 0}, {900.0f, 100} };
    FanCurve c = makeCurve(pts, 2);
    TEST_ASSERT_TRUE(curveValidate(c));
    TEST_ASSERT_EQUAL_FLOAT(0.0f,   c.pts[0].temp);
    TEST_ASSERT_EQUAL_FLOAT(400.0f, c.pts[1].temp);
}

static void test_nan_temp_reads_zero(void)
{
    FanCurve c; curveDefaults(c);
    TEST_ASSERT_TRUE(curveValidate(c));
    // "Temperature unknown" must never spin the fan up.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, curveInterpolate(c, NAN));
}

static void test_nan_point_dropped(void)
{
    const CurvePoint pts[] = { {0.0f, 0}, {NAN, 50}, {200.0f, 100} };
    FanCurve c = makeCurve(pts, 3);
    TEST_ASSERT_TRUE(curveValidate(c));
    TEST_ASSERT_EQUAL_UINT8(2, c.count);
    TEST_ASSERT_EQUAL_FLOAT(50.0f, curveInterpolate(c, 100.0f));
}

static void test_excess_points_truncated(void)
{
    FanCurve c{};
    c.count = CURVE_MAX_POINTS;              // count can never exceed the array
    for (uint8_t i = 0; i < CURVE_MAX_POINTS; i++) {
        c.pts[i].temp = (float)(i * 20);
        c.pts[i].speed = (uint8_t)(i * 6);
    }
    TEST_ASSERT_TRUE(curveValidate(c));
    TEST_ASSERT_EQUAL_UINT8(CURVE_MAX_POINTS, c.count);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, curveInterpolate(c, 20.0f));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_below_first_point);
    RUN_TEST(test_above_last_point);
    RUN_TEST(test_exact_points);
    RUN_TEST(test_midpoint);
    RUN_TEST(test_unsorted_input_is_sorted);
    RUN_TEST(test_duplicate_temps_deduped);
    RUN_TEST(test_fewer_than_two_points_invalid);
    RUN_TEST(test_speed_clamped);
    RUN_TEST(test_temp_clamped);
    RUN_TEST(test_nan_temp_reads_zero);
    RUN_TEST(test_nan_point_dropped);
    RUN_TEST(test_excess_points_truncated);
    return UNITY_END();
}
