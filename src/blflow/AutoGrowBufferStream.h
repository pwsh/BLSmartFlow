// AutoGrowBufferStream.h - a Stream that accumulates everything written to it.
//
// PubSubClient can be handed a Stream to receive payloads that exceed its own
// buffer. Bambu printers push full status reports well past 20 KB, so we let
// this buffer grow instead of raising PubSubClient's fixed RX buffer.
//
// Differences from the widely copied original (which this replaces):
//   * lengths are size_t, not uint16_t - the original silently wrapped at 64 KB
//   * a hard cap (BUFFER_MAX) stops a malformed/hostile payload eating the heap
//   * get_string() no longer writes the NUL past the end of the allocation
//   * every realloc result is checked before the old pointer is overwritten
//   * the buffer doubles instead of creeping up 1 KB at a time: a 20 KB report
//     used to cost twenty reallocs and the memcpy traffic that goes with them
//   * reset() keeps the high-water buffer, so the steady state is zero
//     allocations per message; release() hands it back when the link goes down

#ifndef BLSF_AUTOGROWBUFFERSTREAM_H
#define BLSF_AUTOGROWBUFFERSTREAM_H

#include <Arduino.h>
#include <Stream.h>

class AutoGrowBufferStream : public Stream {
public:
    static const size_t BUFFER_INCREMENTS = 1024;
    static const size_t BUFFER_MAX = 64 * 1024;

    // The buffer is allocated on the first write, so an instance that never
    // receives anything (no printer configured) costs nothing.
    AutoGrowBufferStream()
        : _len(0), _size(0), _buffer(nullptr), _overflow(false) {}

    ~AutoGrowBufferStream() { free(_buffer); }

    AutoGrowBufferStream(const AutoGrowBufferStream&) = delete;
    AutoGrowBufferStream& operator=(const AutoGrowBufferStream&) = delete;

    size_t write(uint8_t byte) override
    {
        if (_len + 1 > _size && !grow(_len + 1)) return 0;
        _buffer[_len++] = (char)byte;
        return 1;
    }

    size_t write(const uint8_t* data, size_t len) override
    {
        if (!data || len == 0) return 0;
        if (_len + len > _size && !grow(_len + len)) {
            // Store whatever still fits: the message is already flagged as
            // overflowed, and a partial buffer keeps current_length() honest.
            len = _size > _len ? _size - _len : 0;
            if (len == 0) return 0;
        }
        memcpy(_buffer + _len, data, len);
        _len += len;
        return len;
    }

    int read() override { return -1; }
    int available() override { return 0; }
    int peek() override { return -1; }
    void flush() override { reset(); }

    // Empties the buffer for the next message. The allocation is deliberately
    // kept: reports are all roughly the same size, so after the first one this
    // makes receiving allocation-free.
    void reset()
    {
        _len = 0;
        _overflow = false;
    }

    // Hands the buffer back to the heap. For teardown, when the link is going
    // down and holding a 64 KB high-water allocation would be pure waste; the
    // next write() allocates again from scratch.
    void release()
    {
        free(_buffer);
        _buffer = nullptr;
        _size = 0;
        _len = 0;
        _overflow = false;
    }

    size_t      current_length() const { return _len; }
    const char* get_buffer() const { return _buffer; }
    bool        overflowed() const { return _overflow; }

    // NUL-terminated view of the contents. Safe because the allocation always
    // has one spare byte beyond _size.
    const char* get_string()
    {
        if (!_buffer) return "";
        _buffer[_len] = '\0';
        return _buffer;
    }

    using Print::write;

private:
    // Grows the buffer to hold at least `need` bytes plus the reserved NUL.
    // Geometric so a large report costs a handful of reallocs, not one per
    // kilobyte, and hard-capped so a hostile payload cannot eat the heap.
    bool grow(size_t need)
    {
        if (need <= _size) return true;
        size_t want = _size ? _size * 2 : BUFFER_INCREMENTS;
        if (want < need) want = need;
        if (want < BUFFER_INCREMENTS) want = BUFFER_INCREMENTS;
        if (want > BUFFER_MAX) want = BUFFER_MAX;
        if (want <= _size) {            // already at the cap
            _overflow = true;
            return false;
        }
        char* tmp = (char*)realloc(_buffer, want + 1);
        if (!tmp) {                     // realloc left the old buffer intact
            _overflow = true;
            return false;
        }
        _buffer = tmp;
        _size = want;
        if (_size < need) {             // the cap cut the request short
            _overflow = true;
            return false;
        }
        return true;
    }

    size_t _len;
    size_t _size;      // usable capacity, excluding the reserved NUL slot
    char*  _buffer;
    bool   _overflow;
};

#endif  // BLSF_AUTOGROWBUFFERSTREAM_H
