// Host stub of the Arduino Print/Stream base classes.
//
// AutoGrowBufferStream is a Stream subclass but its logic - growth, the 64 KB
// cap, NUL-termination - is plain C++ with nothing device-specific in it. These
// two declarations are all it needs from the framework, so the real header can
// be compiled and tested on the host without pulling in Arduino-ESP32.

#ifndef BLSF_TEST_STREAM_H
#define BLSF_TEST_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t b) = 0;
    virtual size_t write(const uint8_t* buf, size_t size)
    {
        size_t n = 0;
        while (n < size && write(buf[n]) == 1) n++;
        return n;
    }
    size_t write(const char* s) { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
};

class Stream : public Print {
public:
    virtual int  read() = 0;
    virtual int  available() = 0;
    virtual int  peek() = 0;
    virtual void flush() {}
};

#endif  // BLSF_TEST_STREAM_H
