// log.h - printf-style logging to Serial plus an in-memory ring buffer.
//
// Every log line is appended to a fixed-size ring buffer (no heap churn) so the
// web UI can show the last few minutes of activity via GET /api/log and receive
// new lines live over SSE. Serial output is gated on config.debug.serial.
//
// The ring buffer is guarded by a spinlock because lines are produced from both
// the Arduino loop task and the printer MQTT task.

#ifndef BLSF_LOG_H
#define BLSF_LOG_H

#include <Arduino.h>
#include <stdarg.h>

namespace blsf {

static const uint8_t  LOG_LINES = 64;
static const uint8_t  LOG_LINE_LEN = 120;   // including the NUL terminator

void logInit();
void logSetSerialEnabled(bool enabled);
bool logSerialEnabled();

// Formats one line, timestamps it and stores it. Called through the macros below.
void logWrite(char level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Copies up to `max` of the most recent lines (oldest first) into `out`.
// Returns how many were written. `out` must be at least max * LOG_LINE_LEN bytes.
uint8_t logSnapshot(char* out, uint8_t max);

// Monotonic counter of lines ever produced; lets the SSE pusher detect new
// lines without polling the buffer contents.
uint32_t logSequence();

// Copies lines produced after `sinceSeq` (at most LOG_LINES). Updates *sinceSeq.
uint8_t logSince(uint32_t* sinceSeq, char* out, uint8_t max);

}  // namespace blsf

#define LOGI(fmt, ...) ::blsf::logWrite('I', fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) ::blsf::logWrite('W', fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) ::blsf::logWrite('E', fmt, ##__VA_ARGS__)

#endif  // BLSF_LOG_H
