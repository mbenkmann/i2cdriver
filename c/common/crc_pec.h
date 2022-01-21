#ifndef _CrC_PEC_H_
#define _CrC_PEC_H_

#include "crc.h"

class CRC_PEC : public CRC
{
    uint8_t crc = 0;
    // static const uint8_t PEC_Table[256];

    static const unsigned POLY = (0x1070U << 3);
    static uint8_t crc8(uint16_t data)
    {
        for (int i = 0; i < 8; i++)
        {
            if (data & 0x8000)
                data = data ^ POLY;
            data = data << 1;
        }
        return (uint8_t)(data >> 8);
    }

  public:
    virtual void set(uint32_t v) { crc = v; };
    virtual uint32_t sum() { return crc; };
    virtual void add(uint8_t b)
    { // crc = PEC_Table[(uint8_t)(crc ^ b)];
        crc = crc8((uint16_t)(crc ^ b) << 8);
    };
    void add(uint8_t* buf, unsigned count)
    {
        for (unsigned i = 0; i < count; i++)
            add(buf[i]);
    }
};

/*
__attribute__((weak)) const uint8_t CRC_PEC::PEC_Table[256] = {
    0,   7,   14,  9,   28,  27,  18,  21,  56,  63,  54,  49,  36,  35,  42,  45,  112, 119, 126, 121, 108, 107,
    98,  101, 72,  79,  70,  65,  84,  83,  90,  93,  224, 231, 238, 233, 252, 251, 242, 245, 216, 223, 214, 209,
    196, 195, 202, 205, 144, 151, 158, 153, 140, 139, 130, 133, 168, 175, 166, 161, 180, 179, 186, 189, 199, 192,
    201, 206, 219, 220, 213, 210, 255, 248, 241, 246, 227, 228, 237, 234, 183, 176, 185, 190, 171, 172, 165, 162,
    143, 136, 129, 134, 147, 148, 157, 154, 39,  32,  41,  46,  59,  60,  53,  50,  31,  24,  17,  22,  3,   4,
    13,  10,  87,  80,  89,  94,  75,  76,  69,  66,  111, 104, 97,  102, 115, 116, 125, 122, 137, 142, 135, 128,
    149, 146, 155, 156, 177, 182, 191, 184, 173, 170, 163, 164, 249, 254, 247, 240, 229, 226, 235, 236, 193, 198,
    207, 200, 221, 218, 211, 212, 105, 110, 103, 96,  117, 114, 123, 124, 81,  86,  95,  88,  77,  74,  67,  68,
    25,  30,  23,  16,  5,   2,   11,  12,  33,  38,  47,  40,  61,  58,  51,  52,  78,  73,  64,  71,  82,  85,
    92,  91,  118, 113, 120, 127, 106, 109, 100, 99,  62,  57,  48,  55,  34,  37,  44,  43,  6,   1,   8,   15,
    26,  29,  20,  19,  174, 169, 160, 167, 178, 181, 188, 187, 150, 145, 152, 159, 138, 141, 132, 131, 222, 217,
    208, 215, 194, 197, 204, 203, 230, 225, 232, 239, 250, 253, 244, 243};
*/
#endif
