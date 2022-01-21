#ifndef _CrC__H_
#define _CrC__H_

#include <stdint.h>

struct CRC
{
    virtual void set(uint32_t) = 0;
    virtual uint32_t sum() = 0;
    virtual void add(uint8_t b) = 0;
    void add(uint8_t* buf, unsigned count)
    {
        for (unsigned i = 0; i < count; i++)
            add(buf[count]);
    }
};

#endif
