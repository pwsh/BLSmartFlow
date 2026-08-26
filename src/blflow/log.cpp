#include "log.h"

namespace blsf {

namespace {
char     g_lines[LOG_LINES][LOG_LINE_LEN];
uint8_t  g_head = 0;        // next slot to write
uint8_t  g_count = 0;       // valid slots, <= LOG_LINES
uint32_t g_seq = 0;         // total lines ever written
bool     g_serial = true;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
}  // namespace

void logInit()
{
    portENTER_CRITICAL(&g_mux);
    g_head = 0;
    g_count = 0;
    g_seq = 0;
    portEXIT_CRITICAL(&g_mux);
}

void logSetSerialEnabled(bool enabled) { g_serial = enabled; }
bool logSerialEnabled() { return g_serial; }

void logWrite(char level, const char* fmt, ...)
{
    // Build the line on the caller's stack: vsnprintf into shared memory while
    // holding a spinlock would keep interrupts disabled far too long.
    char line[LOG_LINE_LEN];
    const uint32_t ms = millis();
    int n = snprintf(line, sizeof(line), "[%7lu] [%c] ", (unsigned long)ms, level);
    if (n < 0) n = 0;
    if ((size_t)n < sizeof(line)) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
        va_end(ap);
    }
    line[sizeof(line) - 1] = '\0';

    if (g_serial) Serial.println(line);

    portENTER_CRITICAL(&g_mux);
    memcpy(g_lines[g_head], line, LOG_LINE_LEN);
    g_head = (uint8_t)((g_head + 1) % LOG_LINES);
    if (g_count < LOG_LINES) g_count++;
    g_seq++;
    portEXIT_CRITICAL(&g_mux);
}

uint8_t logSnapshot(char* out, uint8_t max)
{
    portENTER_CRITICAL(&g_mux);
    const uint8_t count = g_count < max ? g_count : max;
    // Oldest of the `count` lines we are returning.
    uint8_t idx = (uint8_t)((g_head + LOG_LINES - count) % LOG_LINES);
    for (uint8_t i = 0; i < count; i++) {
        memcpy(out + (size_t)i * LOG_LINE_LEN, g_lines[idx], LOG_LINE_LEN);
        idx = (uint8_t)((idx + 1) % LOG_LINES);
    }
    portEXIT_CRITICAL(&g_mux);
    return count;
}

uint32_t logSequence()
{
    portENTER_CRITICAL(&g_mux);
    const uint32_t s = g_seq;
    portEXIT_CRITICAL(&g_mux);
    return s;
}

uint8_t logSince(uint32_t* sinceSeq, char* out, uint8_t max)
{
    portENTER_CRITICAL(&g_mux);
    const uint32_t seq = g_seq;
    uint32_t missed = seq - *sinceSeq;
    // A slow consumer can fall further behind than the buffer holds; give it
    // whatever is still available rather than replaying garbage.
    if (missed > g_count) missed = g_count;
    if (missed > max) missed = max;
    uint8_t count = (uint8_t)missed;
    uint8_t idx = (uint8_t)((g_head + LOG_LINES - count) % LOG_LINES);
    for (uint8_t i = 0; i < count; i++) {
        memcpy(out + (size_t)i * LOG_LINE_LEN, g_lines[idx], LOG_LINE_LEN);
        idx = (uint8_t)((idx + 1) % LOG_LINES);
    }
    *sinceSeq = seq;
    portEXIT_CRITICAL(&g_mux);
    return count;
}

}  // namespace blsf
