// Unity tests for the printer link's RX buffer (src/blflow/AutoGrowBufferStream.h).
//
// The header is a Stream subclass, but everything worth testing - geometric
// growth, the 64 KB cap, NUL-termination, reset/release - is ordinary C++. The
// two stub headers next to this file supply the Print/Stream base classes, so
// the exact code the device runs is exercised here on the host.
//
// Run with: pio test -e native

#include <unity.h>

#include <string.h>
#include <vector>

#include "AutoGrowBufferStream.h"

namespace {

// Writes `n` bytes of a repeating pattern and returns what the stream accepted.
size_t writePattern(AutoGrowBufferStream& s, size_t n, uint8_t seed = 0)
{
    std::vector<uint8_t> data(n);
    for (size_t i = 0; i < n; i++) data[i] = (uint8_t)((i + seed) & 0xFF);
    return s.write(data.data(), n);
}

// True when the first `n` bytes of the buffer match the same pattern.
bool patternMatches(const AutoGrowBufferStream& s, size_t n, uint8_t seed = 0)
{
    const char* buf = s.get_buffer();
    if (!buf) return n == 0;
    for (size_t i = 0; i < n; i++) {
        if ((uint8_t)buf[i] != (uint8_t)((i + seed) & 0xFF)) return false;
    }
    return true;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// --- basics ----------------------------------------------------------------

static void test_starts_empty(void)
{
    AutoGrowBufferStream s;
    TEST_ASSERT_EQUAL_size_t(0, s.current_length());
    TEST_ASSERT_FALSE(s.overflowed());
    // No allocation until something is written.
    TEST_ASSERT_NULL(s.get_buffer());
    TEST_ASSERT_EQUAL_STRING("", s.get_string());
}

static void test_single_byte_writes(void)
{
    AutoGrowBufferStream s;
    for (int i = 0; i < 300; i++) TEST_ASSERT_EQUAL_size_t(1, s.write((uint8_t)('a' + (i % 26))));
    TEST_ASSERT_EQUAL_size_t(300, s.current_length());
    TEST_ASSERT_FALSE(s.overflowed());
    TEST_ASSERT_EQUAL_size_t(300, strlen(s.get_string()));
}

static void test_get_string_is_nul_terminated(void)
{
    AutoGrowBufferStream s;
    const char* json = "{\"print\":{\"gcode_state\":\"RUNNING\"}}";
    TEST_ASSERT_EQUAL_size_t(strlen(json), s.write((const uint8_t*)json, strlen(json)));
    // The allocation always keeps one spare byte, so this cannot run off the end.
    TEST_ASSERT_EQUAL_STRING(json, s.get_string());
}

// --- growth ----------------------------------------------------------------

static void test_grows_past_the_base_size(void)
{
    AutoGrowBufferStream s;
    // Well past BUFFER_INCREMENTS: the old implementation crept up 1 KB at a
    // time, this one doubles, but either way the content must survive intact.
    const size_t n = 20 * 1024;
    TEST_ASSERT_EQUAL_size_t(n, writePattern(s, n));
    TEST_ASSERT_EQUAL_size_t(n, s.current_length());
    TEST_ASSERT_FALSE(s.overflowed());
    TEST_ASSERT_TRUE(patternMatches(s, n));
}

static void test_growth_across_many_small_writes(void)
{
    AutoGrowBufferStream s;
    // A real payload arrives in PubSubClient-sized chunks, not one big block.
    const size_t chunk = 128;
    const size_t chunks = 100;
    for (size_t i = 0; i < chunks; i++) {
        std::vector<uint8_t> data(chunk, (uint8_t)i);
        TEST_ASSERT_EQUAL_size_t(chunk, s.write(data.data(), chunk));
    }
    TEST_ASSERT_EQUAL_size_t(chunk * chunks, s.current_length());
    TEST_ASSERT_FALSE(s.overflowed());
    const char* buf = s.get_buffer();
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)buf[0]);
    TEST_ASSERT_EQUAL_UINT8(chunks - 1, (uint8_t)buf[chunk * chunks - 1]);
}

// --- the cap ---------------------------------------------------------------

static void test_cap_is_enforced_bulk(void)
{
    AutoGrowBufferStream s;
    const size_t over = AutoGrowBufferStream::BUFFER_MAX + 5000;
    const size_t written = writePattern(s, over);
    // Everything up to the cap is kept, the rest is refused and flagged.
    TEST_ASSERT_EQUAL_size_t(AutoGrowBufferStream::BUFFER_MAX, written);
    TEST_ASSERT_EQUAL_size_t(AutoGrowBufferStream::BUFFER_MAX, s.current_length());
    TEST_ASSERT_TRUE(s.overflowed());
    TEST_ASSERT_TRUE(patternMatches(s, AutoGrowBufferStream::BUFFER_MAX));
}

static void test_cap_is_enforced_byte_by_byte(void)
{
    AutoGrowBufferStream s;
    TEST_ASSERT_EQUAL_size_t(AutoGrowBufferStream::BUFFER_MAX,
                             writePattern(s, AutoGrowBufferStream::BUFFER_MAX));
    TEST_ASSERT_FALSE(s.overflowed());
    // Exactly at the cap: the next byte cannot be stored.
    TEST_ASSERT_EQUAL_size_t(0, s.write((uint8_t)'x'));
    TEST_ASSERT_TRUE(s.overflowed());
    TEST_ASSERT_EQUAL_size_t(AutoGrowBufferStream::BUFFER_MAX, s.current_length());
}

static void test_writes_after_overflow_are_refused(void)
{
    AutoGrowBufferStream s;
    writePattern(s, AutoGrowBufferStream::BUFFER_MAX + 1);
    TEST_ASSERT_TRUE(s.overflowed());
    std::vector<uint8_t> more(64, 'z');
    TEST_ASSERT_EQUAL_size_t(0, s.write(more.data(), more.size()));
    TEST_ASSERT_EQUAL_size_t(AutoGrowBufferStream::BUFFER_MAX, s.current_length());
}

// --- reset / release -------------------------------------------------------

static void test_reset_empties_but_keeps_the_buffer(void)
{
    AutoGrowBufferStream s;
    writePattern(s, 20 * 1024);
    const char* before = s.get_buffer();
    s.reset();
    TEST_ASSERT_EQUAL_size_t(0, s.current_length());
    TEST_ASSERT_FALSE(s.overflowed());
    // The high-water allocation is deliberately kept, so the steady state is
    // zero reallocs per message.
    TEST_ASSERT_EQUAL_PTR(before, s.get_buffer());

    // And it is reusable without growing again.
    TEST_ASSERT_EQUAL_size_t(20 * 1024, writePattern(s, 20 * 1024, 7));
    TEST_ASSERT_EQUAL_PTR(before, s.get_buffer());
    TEST_ASSERT_TRUE(patternMatches(s, 20 * 1024, 7));
}

static void test_reset_clears_the_overflow_flag(void)
{
    AutoGrowBufferStream s;
    writePattern(s, AutoGrowBufferStream::BUFFER_MAX + 1);
    TEST_ASSERT_TRUE(s.overflowed());
    s.reset();
    TEST_ASSERT_FALSE(s.overflowed());
    // A following, sane message must be accepted normally.
    TEST_ASSERT_EQUAL_size_t(64, writePattern(s, 64));
    TEST_ASSERT_FALSE(s.overflowed());
}

static void test_flush_behaves_like_reset(void)
{
    AutoGrowBufferStream s;
    writePattern(s, 4096);
    s.flush();
    TEST_ASSERT_EQUAL_size_t(0, s.current_length());
    TEST_ASSERT_FALSE(s.overflowed());
}

static void test_release_hands_the_buffer_back(void)
{
    AutoGrowBufferStream s;
    writePattern(s, 20 * 1024);
    s.release();
    TEST_ASSERT_EQUAL_size_t(0, s.current_length());
    TEST_ASSERT_FALSE(s.overflowed());
    TEST_ASSERT_NULL(s.get_buffer());
    TEST_ASSERT_EQUAL_STRING("", s.get_string());

    // Writing after a release allocates from scratch rather than crashing.
    TEST_ASSERT_EQUAL_size_t(128, writePattern(s, 128, 3));
    TEST_ASSERT_NOT_NULL(s.get_buffer());
    TEST_ASSERT_TRUE(patternMatches(s, 128, 3));
}

static void test_empty_write_is_a_noop(void)
{
    AutoGrowBufferStream s;
    TEST_ASSERT_EQUAL_size_t(0, s.write((const uint8_t*)nullptr, 0));
    TEST_ASSERT_EQUAL_size_t(0, s.write((const uint8_t*)"x", 0));
    TEST_ASSERT_EQUAL_size_t(0, s.current_length());
    TEST_ASSERT_FALSE(s.overflowed());
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_single_byte_writes);
    RUN_TEST(test_get_string_is_nul_terminated);
    RUN_TEST(test_grows_past_the_base_size);
    RUN_TEST(test_growth_across_many_small_writes);
    RUN_TEST(test_cap_is_enforced_bulk);
    RUN_TEST(test_cap_is_enforced_byte_by_byte);
    RUN_TEST(test_writes_after_overflow_are_refused);
    RUN_TEST(test_reset_empties_but_keeps_the_buffer);
    RUN_TEST(test_reset_clears_the_overflow_flag);
    RUN_TEST(test_flush_behaves_like_reset);
    RUN_TEST(test_release_hands_the_buffer_back);
    RUN_TEST(test_empty_write_is_a_noop);
    return UNITY_END();
}
