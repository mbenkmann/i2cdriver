/*   Copyright (C) 2022  Matthias S. Benkmann

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
*/

#define FUSE_USE_VERSION 31

#include <errno.h>
#include <glob.h>
#include <inttypes.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "file.h"
#include <crc_pec.h>
#include <cuse_lowlevel.h>
#include <optionparser.h>

const char* tty = nullptr;
File i2cd("/dev/null");
char monitor = 255;
char speed = 255;
char pullups[2] = {(char)255, (char)255};
struct i2c_msg msgs[I2C_RDWR_IOCTL_MAX_MSGS];
int nmsgs;
bool add_pec = false;

bool parse_transfer(int argc, const char* argv[], struct i2c_msg (&msgs)[I2C_RDWR_IOCTL_MAX_MSGS], int& nmsgs);

uint64_t micros()
{
    static uint64_t starttime(0);
    struct timeval tv;
    gettimeofday(&tv, 0);
    uint64_t now = (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    if (starttime == 0)
        starttime = now;
    return now - starttime;
}

struct Arg : public option::Arg
{
    static const char* const BAUD_LIST[];
    static const char* const PULLUP_LIST[];

    static int index(const char* arg, const char* const list[])
    {
        if (arg != nullptr)
        {
            for (int i = 0; list[i] != 0; i++)
                if (strcmp(list[i], arg) == 0)
                    return i;
        }
        return -1;
    }

    static void printError(const char* msg1, const option::Option& opt, const char* msg2)
    {
        fprintf(stderr, "%s", msg1);
        fwrite(opt.name, opt.namelen, 1, stderr);
        fprintf(stderr, "%s", msg2);
    }

    static option::ArgStatus Unknown(const option::Option& option, bool msg)
    {
        if (msg)
            printError("Unknown option '", option, "'\n");
        return option::ARG_ILLEGAL;
    }

    static int Int7(const char* arg)
    {
        char* endptr = 0;
        long l = -1;
        if (arg != 0)
        {
            const char* num = arg;
            // prevent interpretation as octal
            while (num[0] != 0 && num[1] != 0 && num[1] != 'x' && num[1] != 'X' && num[0] == '0')
                num++;
            l = strtol(num, &endptr, 0);
        }

        if (endptr != arg && *endptr == 0 && l >= 0 && l < 128)
            return l;

        return -1;
    }

    static option::ArgStatus NonNegative(const option::Option& option, bool msg)
    {
        char* endptr = 0;
        long l = -1;
        if (option.arg != 0)
            l = strtol(option.arg, &endptr, 10);

        if (endptr != option.arg && *endptr == 0 && l >= 0)
            return option::ARG_OK;

        if (msg)
            printError("Option '", option, "' requires a number greater or equal 0\n");
        return option::ARG_ILLEGAL;
    }

    static option::ArgStatus Required(const option::Option& option, bool msg)
    {
        if (option.arg != 0)
            return option::ARG_OK;

        if (msg)
            printError("Option '", option, "' requires an argument\n");
        return option::ARG_ILLEGAL;
    }

    static option::ArgStatus Baud(const option::Option& option, bool msg)
    {
        if (index(option.arg, BAUD_LIST) >= 0)
            return option::ARG_OK;

        if (msg)
            printError("Option '", option, "' requires as argument '100' or '400'\n");
        return option::ARG_ILLEGAL;
    }

    static option::ArgStatus Pullups(const option::Option& option, bool msg)
    {
        if (index(option.arg, PULLUP_LIST) >= 0)
            return option::ARG_OK;

        if (msg)
            printError("Option '", option, "' requires as argument '0', '1.1', '1.5', '2.2', '4.3' or '4.7'\n");
        return option::ARG_ILLEGAL;
    }
};

const char* const Arg::BAUD_LIST[] = {"100", "400", 0};
const char* const Arg::PULLUP_LIST[] = {"0", "2.2", "4.3", "1.5", "4.7", "1.5", "2.2", "1.1", 0};

enum optionIndex
{
    UNKNOWN,
    HELP,
    DEV,
    BACKGROUND,
    TTY,
    PULLUPS,
    KHZ,
    LATENCY,
    RESET,
    REBOOT,
    INFO,
    SCAN,
    MONITOR,
    CAPTURE,
    TRANSFER,
    PEC,
};
const option::Descriptor usage[] = {
    {UNKNOWN, 0, "", "", Arg::Unknown,
     "== i2cdriver universal control program ==\n"
     "   (c) 2022 Matthias S.Benkmann\n\n"
     "OPTIONS:\nLong options can be abbreviated to any unique prefix.\n"},
    {HELP, 0, "", "help", Arg::None, "  \t--help  \tPrint usage and exit."},
    {DEV, 0, "d", "dev", Arg::Required,
     "  -d[<name>], \t--dev[=<name>]"
     "  \t(requires /dev/cuse permissions) create /dev/<name> to emulate a /dev/i2c-... bus device."},
    {BACKGROUND, 0, "b", "background", Arg::None, "  -b, \t--background  \tHandle --dev in the background."},
    {TTY, 0, "t", "tty", Arg::Required,
     "  -t <ttypath>, \t--tty=<ttypath>  \tPath to the ttyUSB device. Not required if there is only 1 possibility."},
    {PULLUPS, 0, "p", "pullups", Arg::Pullups,
     "  -p <kOhm>, \t--pullups=<kOhm>"
     "  \tSet pullups for SCL and SDA. Values are 0, 1.1, 1.5, 2.2, 4.3, 4.7 ."},
    {KHZ, 0, "k", "kHz", Arg::Baud,
     "  -k 100|400, \t--kHz=100|400"
     "  \tSet clock rate to 100kHz or 400kHz."},
    {LATENCY, 0, "", "ll", Arg::None,
     "  \t--ll"
     "  \tSet USB latency to minimum."},
    {RESET, 0, "", "reset", Arg::None,
     "  \t--reset"
     "  \tAttempt to unblock confused I2C devices to free up the I2C bus."},
    {REBOOT, 0, "", "reboot", Arg::None,
     "  \t--reboot"
     "  \tReboot the I2CDriver hardware. Takes half a second."},
    {INFO, 0, "i", "info", Arg::None,
     " -i, \t--info"
     "  \tPrint out the current status and configuration of the I2CDriver."},
    {SCAN, 0, "s", "scan", Arg::None,
     " -s, \t--scan"
     "  \tTry to read from all I2C addresses and print out addresses on which a device replies."},
    {MONITOR, 0, "m", "monitor", Arg::None,
     " -m, \t--monitor"
     "  \tSwitch the I2CDriver to monitor mode after all transmissions and capturing are done."},
    {CAPTURE, 0, "c", "capture", Arg::NonNegative,
     " -c <secs>, \t--capture=<secs>"
     "  \tAfter all transmissions, capture events for <secs> seconds and decode them to stdout."},
    {TRANSFER, 0, "", "transfer", Arg::Required,
     "  \t--transfer=<data>"
     "  \tPerform I2C transfer(s) according to <data>. See below for details."},
    {PEC, 0, "", "pec", Arg::None,
     "  \t--pec"
     "  \tAttach a Packet Error Checking byte to each subsequent write-only --transfer datastream. Report PEC for read "
     "and write --transfers."},
    {UNKNOWN, 0, "", "", Arg::None,
     "\nTRANSFER DATA STRING:\n"
     "A transfer may consist of multiple messages and is started with a START condition and ends with a STOP "
     "condition. Messages within the transfer are concatenated using a REPEATED START condition.\n\n"
     "Messages and message data within the transfer data string are separated by whitespace or ','.\n\n"
     "Each message begins with a descriptor of the following format:\n\n"
     "    {r|w}<length_of_message>[@address]   Example: r3@0x50\n"},
    {UNKNOWN, 0, "", "", Arg::None,
     "\n        {r|w}\t  'r' indicates a read (device->PC);\v  'w' indicates a write (PC->device).\n"
     "    <length_of_message>\t  Number of bytes to read/write (0-65535).\n"
     "          ?\t  '?' as length for a read message causes 1 byte to be read and that byte determines\v  how many "
     "more "
     "bytes will be read.\n"
     "      @address\t  Address of the target device. Only required for the first message.\v  If omitted, the address "
     "from the prior message will be reused.\n\n"
     "If the I2C message is a write, then a data block specifying the required number of bytes must follow the "
     "descriptor. It consists of <length_of_message> numbers, separated by whitespace or ','.  To make it easier to "
     "create larger data blocks, a number may have a suffix:\n\n"
     "  =      keep value constant until end of message (i.e. 0= means 0, 0, 0, ...)\n\n"
     "  +      increase value by 1 until end of message (i.e. 0+ means 0, 1, 2, ...)\n\n"
     "  -      decrease value by 1 until end of message (i.e. 0xff- means 0xff, 0xfe, 0xfd, ...)\n\n"
     "  p      use value as seed for an 8 bit pseudo random sequence (i.e. 0p means 0x00, 0x50, 0xb0, ...)\n"
     "\n"},
    {UNKNOWN, 0, "", "", Arg::None,
     "EXAMPLES:\n"
     "  i2cdriver --kHz=100 --pullups=0 --ll --tty=/dev/ttyUSB0 --info\n"
     "  i2cdriver --transfer=\"w2@0x50 0x12 0x34, r2\"\n"
     "  i2cdriver --transfer=w2@80,18,52,r2\n"
     "  i2cdriver --transfer=\"r?@0x77\"\n"
     "  i2cdriver --transfer=\"w1024@0x77 0p\"\n"
     "  i2cdriver --capture=1000\n"},
    {0, 0, 0, 0, 0, 0}};

// Checks path if it looks like a TTY and if we can access it. Returns a
// resolved path with all symlinks and "."/".." resolved.
const char* sanityCheckTTY(const char* path)
{
    char* tty = nullptr;
    struct stat st;
    if (0 == stat(path, &st))
    {
        // test if it's a char device
        if (S_ISCHR(st.st_mode))
        {
            // test if we can read/write it
            int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (fd >= 0)
            {
                close(fd);
                tty = realpath(path, nullptr);
            }
        }
    }

    return tty;
}

const char* autodetectTTY()
{
    const char* tty = nullptr;
    glob_t g;
    if (0 == glob("/dev/ttyUSB*", GLOB_NOSORT, nullptr, &g))
    {
        for (unsigned i = 0; i < g.gl_pathc; i++)
        {
            const char* tty2 = sanityCheckTTY(g.gl_pathv[i]);
            if (tty2 != nullptr)
            {
                if (tty != nullptr) // if we have more than 1 candidate
                {
                    tty = nullptr;
                    break;
                }
                tty = tty2;
            }
        }
    }
    globfree(&g);

    return tty;
}

char* getLatencyTimer(const char* tty)
{
    const char* name = strrchr(tty, '/');
    if (name == nullptr)
        name = tty;
    else
        name++;
    char* syspath;
    if (asprintf(&syspath, "/sys/bus/usb-serial/devices/%s/latency_timer", name) < 0)
        return nullptr;
    return syspath;
}

bool setLowLatency(const char* tty)
{
    auto syspath = getLatencyTimer(tty);
    File sysfile(syspath);
    sysfile.action("setting USB latency");
    sysfile.open();
    sysfile.writeAll("1", 1);
    if (sysfile.hasError())
        fprintf(stderr, "%s\n", sysfile.error());
    sysfile.close();
    free(syspath);
    return !sysfile.hasError();
}

int getUSBLatency(const char* tty)
{
    auto syspath = getLatencyTimer(tty);
    File sysfile(syspath);
    sysfile.action("getting USB latency");
    sysfile.open(O_RDONLY | O_NONBLOCK);
    char buf[10];
    int l = sysfile.read(buf, sizeof(buf) - 1);
    sysfile.close();
    free(syspath);
    if (l > 0)
    {
        buf[l] = 0;
        char* endptr;
        auto lat = strtol(buf, &endptr, 10);
        if (endptr != buf && *endptr <= ' ')
            return lat;
    }
    return -1;
}

const char* decode_pullup(int p)
{
    p &= 0b111;
    return Arg::PULLUP_LIST[p];
}

void resetBus()
{
    i2cd.action("resetting bus");
    i2cd.writeAll("x", 1);
    char result;
    bool success = (i2cd.read(&result, 1, 0, -1, 500) > 0 && result == '3');
    fprintf(stdout, "Bus reset %s\n", success ? "SUCCESSFUL" : "FAILED");
}

// Returns:
// 0: if the I2CDriver is ready
// 1: if the I2CDriver is not ready (but there are no I/O errors)
// -1: if an I/O error occured accessing the TTY
int waitReady()
{
    int i;
    for (i = 0; i < 100; i++)
    {
        char buf[4];
        i2cd.writeAll("@i e\n", 5);
        if (i2cd.read(buf, sizeof(buf), 5, 100, 50) > 0 && buf[0] == '\n')
        {
            i2cd.writeAll("e\0", 2);
            if (i2cd.read(buf, sizeof(buf), 5, 100, 50) > 0 && buf[0] == '\0')
                return 0;
        }
        if (i2cd.hasError() && i2cd.errNo() != EWOULDBLOCK)
            return -1;
        i2cd.clearError();
    }
    return 1;
}

void reboot()
{
    i2cd.action("rebooting device");
    i2cd.writeAll("_", 1);
    usleep(500000);
    bool success = (waitReady() == 0);
    fprintf(stdout, "I2CDriver reboot %s\n", success ? "SUCCESSFUL" : "FAILED");
}

void scan()
{
    i2cd.action("scanning bus");
    i2cd.writeAll("d", 1);
    char buf[200];
    int sz = i2cd.read(buf, sizeof(buf), 20, -1, 500);
    if (sz == 112)
    {
        buf[sz] = 0;
        for (int i = 0; i < 112; ++i)
        {
            switch (buf[i])
            {
                case '0':
                    break;
                case '1':
                    fprintf(stdout, "0x%X ACK\n", i + 8);
                    break;
                default:
                    fprintf(stdout, "0x%X ???\n", i + 8);
                    break;
            }
        }
    }
}

void maybeSet(unsigned char ch)
{
    if (ch != 255)
    {
        i2cd.writeAll(&ch, 1);
        i2cd.writeAll("\n", 1);
        i2cd.writeAll(&ch, 1);
    }
}

void maybeSet2(unsigned char ch, unsigned char ch2)
{
    if (ch != 255)
    {
        i2cd.writeAll(&ch, 1);
        i2cd.writeAll(&ch2, 1);
        i2cd.writeAll("\n", 1);
        i2cd.writeAll(&ch, 1);
        i2cd.writeAll(&ch2, 1);
    }
}

struct Color
{
    const char* ERR = "\x1B[1;31m\x1B[7m";
    const char* START = "\x1B[33m\x1B[7m";
    const char* STOP = "\x1B[36m\x1B[7m";
    const char* RW = START;
    const char* DEFAULT = "\x1B[0m";
    const char* DATA = "\x1B[1;37m";
    const char* ACK = "\x1B[1;32m";
    const char* NACK = "\x1B[1;31m";
    const char* ADDR = "\x1B[0;1m\x1B[7m";
} color;

void decodeCapture(uint8_t data)
{
    const char* ACKNACK[2] = {color.ACK, color.NACK};
    const char acknack[2] = {'.', '\''};
    static int count = 0;
    static unsigned cur = 0;
    static bool inData = false;

    for (int i = 4; i >= 0; i -= 4)
    {
        uint8_t b = (data >> i) & 0xF;

        bool idle = (count == 0);

        if (count > 0 && b < 8)
        { // premature flush
            fprintf(stdout, "%s%2X/%d%s\n", color.ERR, cur, count * 3, color.DEFAULT);
            cur = 0;
            count = 0;
            inData = false;
        }

        if (b >= 8)
        {
            cur = (cur << 3) | (b - 8);
            if (++count == 3)
            { // 3 triplets => 1 byte done
                int ack = cur & 1;
                cur >>= 1;
                if (!inData)
                { // byte is address
                    char rw = (cur & 1) == 0 ? 'W' : 'R';
                    cur >>= 1;
                    fprintf(stdout, "%s%c%s", color.RW, rw, color.ADDR);
                }
                else
                    fprintf(stdout, "%s", color.DATA);

                fprintf(stdout, "%02X%s%s%c%s", cur, color.DEFAULT, ACKNACK[ack], acknack[ack], color.DEFAULT);

                cur = 0;
                count = 0;
                inData = true;
            }
        }
        else if (b == 0) // Bus IDLE
        {
            if (!idle)
                fprintf(stdout, "\n");
            inData = false;
        }
        else if (b == 2) // STOP
        {
            fprintf(stdout, "%sP%s\n", color.STOP, color.DEFAULT);
            inData = false;
        }
        else if (b == 1) // START
        {
            fprintf(stdout, "%sS%s", color.START, color.DEFAULT);
            inData = false;
        }
        else if (b >= 3 && b < 8) // undocumented code
        {
            fprintf(stdout, "%s%x%s ", color.ERR, b, color.DEFAULT);
        }
    }
}

void i2c_rdwr_dump(struct i2c_rdwr_ioctl_data& rdwr, bool dumpwrites, uint8_t pec = 0)
{
    uint8_t delayed = 255;
    decodeCapture(1);
    for (unsigned i = 0; i < rdwr.nmsgs; i++)
    {
        struct i2c_msg& msg = rdwr.msgs[i];
        if (msg.buf == nullptr)
            continue; // should not happen
        if ((msg.flags & I2C_M_RD) == 0 && !dumpwrites)
            continue;
        int len = (msg.flags & I2C_M_RECV_LEN) ? msg.buf[0] + 1 : msg.len;
        int rd = (msg.flags & I2C_M_RD);
        int addr = ((msg.addr & 0x7f) << 1) | rd;
        addr <<= 1;
        uint8_t a = 8 + (addr >> 6);
        uint8_t b = 8 + ((addr >> 3) & 7);
        uint8_t c = 8 + (addr & 7);
        uint8_t* buf = msg.buf;
        if (delayed == 0)
        {
            decodeCapture((1 << 4) | a);
            decodeCapture((b << 4) | c);
            goto skip;
        }
        else if (delayed != 255)
        {
            decodeCapture(delayed);
        }

        while (1)
        {
            decodeCapture((a << 4) | b);

            if (len == 0)
            {
                delayed = (c << 4) | 1;
                break;
            }

            a = 8 + (*buf >> 5);
            decodeCapture((c << 4) | a);
            b = 8 + ((*buf >> 2) & 7);
            c = 8 + ((*buf << 1) & 7) + (rd & (len == 1));
            buf++;
            len--;

            decodeCapture((b << 4) | c);
        skip:
            if (len == 0)
            {
                delayed = 0;
                break;
            }

            a = 8 + (*buf >> 5);
            b = 8 + ((*buf >> 2) & 7);
            c = 8 + ((*buf << 1) & 7) + (rd & (len == 1));
            buf++;
            len--;
        }
    }
    if (delayed == 0 || delayed == 255)
        decodeCapture(0x20);
    else
        decodeCapture(delayed + 1);
    if (add_pec)
        fprintf(stdout, "PEC: 0x%02X", pec);
    fprintf(stdout, "%s\n", color.DEFAULT);
}

// Reads up to 2 bytes from i2cd and returns true if either no byte was
// received or any received byte is not 0b110001 (the OK response).
bool i2cdriverErr()
{
    uint8_t buf[2];
    int res = i2cd.read(buf, 2, 0, -1, 30);
    if (res <= 0 || buf[0] != 0b110001 || (res > 1 && buf[1] != 0b110001))
        return true;
    return false;
}

bool i2c_rdwr(struct i2c_rdwr_ioctl_data& rdwr)
{
    if (rdwr.nmsgs == 0)
        return true;

    CRC_PEC pec;
    uint8_t buf[32];
    bool ioerror = false;
    bool do_add_pec = add_pec;

    i2cd.read(buf, sizeof(buf), 0, 0); // clear input buffer
    i2cd.clearError();                 // clear EWOULDBLOCK if nothing was read

    for (unsigned i = 0; i < rdwr.nmsgs; i++)
    {
        struct i2c_msg& msg = rdwr.msgs[i];
        if (msg.buf == nullptr)
            continue; // should not happen

        int len = msg.len;

        // 0-length writes are not permitted. Convert to 0-length read.
        if (len == 0)
            msg.flags = (msg.flags | I2C_M_RD) & ~I2C_M_RECV_LEN;

        buf[0] = 's';                                      // START command
        buf[1] = (msg.addr << 1) | (msg.flags & I2C_M_RD); // address for START command with R/W bit
        i2cd.action("I2C START");
        i2cd.writeAll(buf, 2);
        pec.add(buf[1]); // use the time it takes to process the write to calculate the PEC
        ioerror = i2cdriverErr();
        if (ioerror)
            goto endoftransmission;

        int datidx = 0;

        if (msg.flags & I2C_M_RD) // Read
        {
            do_add_pec = false; // if there's a single read in the transaction, we don't add a PEC
            i2cd.action("I2C read");

            if (msg.flags & I2C_M_RECV_LEN) // length in first byte received
            {
                buf[0] = 'a'; // i2cdriver read-all-ACK command
                buf[1] = 1;
                i2cd.writeAll(buf, 2);
                ioerror = (1 != i2cd.read(msg.buf + datidx, 1, 30, -1, 100));
                if (ioerror)
                    goto endoftransmission;

                pec.add((uint8_t)msg.buf[datidx]);

                len = (uint8_t)msg.buf[datidx];
                datidx += 1;
            }

            if (len == 0)
                goto endoftransmission;

            while (len > 64) // use i2cdriver's 'a' command until we have <=64 bytes left
            {
                int l = len > 255 ? 255 : len - 1; // -1 to make sure we have at least 1 byte left to NACK
                buf[0] = 'a';                      // i2cdriver read-all-ACK command
                buf[1] = l;
                i2cd.writeAll(buf, 2);
                ioerror = (l != i2cd.read(msg.buf + datidx, l, 30, -1, 100));
                if (ioerror)
                    goto endoftransmission;

                pec.add(msg.buf + datidx, l);

                len -= l;
                datidx += l;
            }

            // at this point 1 <= len <= 64
            buf[0] = (len - 1) | 0b10000000; // i2cdriver read-with-final-NACK command
            i2cd.writeAll(buf, 1);
            ioerror = (len != i2cd.read(msg.buf + datidx, len, 30, -1, 100));
            if (ioerror)
                goto endoftransmission;

            pec.add(msg.buf + datidx, len);
        }
        else // Write
        {
            i2cd.action("I2C write");

            while (len > 0)
            {
                int l = len > 64 ? 64 : len;
                buf[0] = (l - 1) | 0b11000000; // i2cdriver write command
                i2cd.writeAll(buf, 1);
                i2cd.writeAll(msg.buf + datidx, l);
                pec.add(msg.buf + datidx, l); // use the time it takes to process the write to calculate the PEC
                ioerror = i2cdriverErr();
                if (ioerror)
                    goto endoftransmission;

                len -= l;
                datidx += l;
            }
        }
    }

endoftransmission:
    if (do_add_pec && !ioerror)
    {
        buf[0] = 0b11000000; // i2cdriver write command
        buf[1] = pec.sum();
        i2cd.action("I2C STOP");
        i2cd.writeAll(buf, 2);
        ioerror = i2cdriverErr();
    }
    i2cd.writeAll("p", 1); // STOP

    i2c_rdwr_dump(rdwr, true, pec.sum());

    return !ioerror;
}

void transfer(const char* carg)
{
    char* vararg = strdup(carg);
    char* arg = vararg;
    int argc = 0;
    static const int max = 1024;
    const char* argv[max];

    while (argc < max - 1)
    {
        while (*arg != 0 && (*arg <= ' ' || *arg == ','))
            *arg++ = 0;
        if (*arg == 0)
            break;
        argv[argc++] = arg;
        while (*arg != 0 && *arg > ' ' && *arg != ',')
            ++arg;
    }

    argv[argc] = nullptr;

    if (parse_transfer(argc, argv, msgs, nmsgs))
    {
        struct i2c_rdwr_ioctl_data rdwr;
        rdwr.msgs = msgs;
        rdwr.nmsgs = nmsgs;
        if (!i2c_rdwr(rdwr))
        {
            fprintf(stderr, "I/O Error or No reply during transmission\n");
        }
    }

    free(vararg);
}

int main(int argc, char* argv[])
{
    for (int i = 0; i < I2C_RDWR_IOCTL_MAX_MSGS; i++)
        msgs[i].buf = NULL;

    argc -= (argc > 0);
    argv += (argc > 0); // skip program name argv[0] if present
    option::Stats stats(usage, argc, argv, 1);

    option::Option* options = new option::Option[stats.options_max];
    option::Option* buffer = new option::Option[stats.buffer_max];

    option::Parser parse(usage, argc, argv, options, buffer, 1);

    if (parse.error())
        return 1;

    if (parse.nonOptionsCount() > 0)
    {
        fprintf(stderr, "Illegal argument: %s\n", parse.nonOption(0));
        return 1;
    }

    if (options[HELP] || argc == 0)
    {
        int columns = getenv("COLUMNS") ? atoi(getenv("COLUMNS")) : 80;
        option::printUsage(fwrite, stdout, usage, columns);
        return 0;
    }

    if (options[DEV].count() > 1)
    {
        fprintf(stderr, "At most one --dev argument is allowed\n");
        return 1;
    }

    switch (options[TTY].count())
    {
        case 0: // auto-detect
            tty = autodetectTTY();
            if (tty == nullptr)
            {
                fprintf(stderr, "Could not autodetect I2Cdriver device. Please pass --tty option.\n");
                return 1;
            }
            break;
        case 1: // verify
            tty = sanityCheckTTY(options[TTY].arg);
            if (tty == nullptr)
            {
                fprintf(stderr, "%s does not look like a valid I2Cdriver device.\n", options[TTY].arg);
                return 1;
            }
            break;
        default:
            fprintf(stderr, "At most one --tty argument is allowed.\n");
            return 1;
    }

    if (options[LATENCY])
    {
        if (!setLowLatency(tty))
            return 1;
    }

    if (options[DEV])
    {
        int fd = open("/dev/cuse", O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0)
        {
            if (errno == ENOENT || errno == ENXIO)
            {
                fprintf(stderr, "/dev/cuse not found. Does your kernel support it? You may need to 'modprobe cuse'.\n");
                return 1;
            }
            perror("/dev/cuse");
            return 1;
        }
        close(fd);
    }

    i2cd.init(tty);
    i2cd.action("connecting to TTY");
    i2cd.open();
    i2cd.setupTTY(B1000000);

    switch (waitReady())
    {
        case 0:
            break;
        case -1:
            fprintf(stderr, "%s\n", i2cd.error());
            return 1;
        default:
            fprintf(
                stderr,
                "Protocol failure. Is %s really an I2CDriver?\nIt could also be that your I2C wires are connected to "
                "an unpowered circuit. This can cause issues.\n",
                tty);
            return 1;
    }

    for (int i = 0; i < parse.optionsCount(); ++i)
    {
        option::Option& opt = buffer[i];
        switch (opt.index())
        {
            case PULLUPS:
                pullups[0] = 'u';
                pullups[1] = Arg::index(opt.arg, Arg::PULLUP_LIST);
                pullups[1] = pullups[1] | (pullups[1] << 3);
                break;
            case KHZ:
                speed = "14"[Arg::index(opt.arg, Arg::BAUD_LIST)];
                break;
            case PEC:
                add_pec = true;
                break;
            case MONITOR:
                monitor = 'm';
                break;
            case RESET:
                resetBus();
                break;
            case REBOOT:
                reboot();
                break;
            case SCAN:
                maybeSet(speed);
                maybeSet2(pullups[0], pullups[1]);
                scan();
                break;

            case TRANSFER:
                maybeSet(speed);
                maybeSet2(pullups[0], pullups[1]);
                transfer(opt.arg);
                break;

            case INFO:       // we only print info once after handling everything
            case DEV:        // will be handled later
            case BACKGROUND: // ditto
            case CAPTURE:    // will be handled later
            case TTY:        // already handled
            case LATENCY:    // already handled
            case HELP:       // not possible, because handled further above and exits the program
            case UNKNOWN:    // not possible because Arg::Unknown aborts the parse with an error
                break;
        }

        if (i2cd.hasError())
            break;
    }

    maybeSet(speed);
    maybeSet2(pullups[0], pullups[1]);

    if (options[INFO])
    {
        char model[16];
        char serial[9];         // Serial number of USB device
        uint64_t uptime;        // time since boot (seconds)
        float voltage_v;        // USB voltage (Volts)
        float current_ma;       // device current (mA)
        float temp_celsius;     // temperature (C)
        char mode;              // I2C 'I' or bitbang 'B' mode
        unsigned int sda;       // SDA state, 0 or 1
        unsigned int scl;       // SCL state, 0 or 1
        unsigned int speed;     // I2C line speed (in kHz)
        unsigned int pullups;   // pullup state (6 bits, 1=enabled)
        unsigned int ccitt_crc; // Hardware CCITT CRC
        int usb_latency = getUSBLatency(tty);

        char buf[100];
        i2cd.action("obtaining i2cdriver status");
        i2cd.writeAll("?", 1);
        int sz = i2cd.read(buf, sizeof(buf), 20, 1000, 20);
        if (sz > 20)
        {
            buf[sz] = 0;
            if (12 == sscanf(buf, "[%15s %8s %" SCNu64 " %f %f %f %c %d %d %d %x %x ]", model, serial, &uptime,
                             &voltage_v, &current_ma, &temp_celsius, &mode, &sda, &scl, &speed, &pullups, &ccitt_crc))
            {

                fprintf(stdout, "USB latency: ");
                if (usb_latency <= 0)
                    fprintf(stdout, "Unknown\n");
                else
                    fprintf(stdout, "%dms\n", usb_latency);

                fprintf(
                    stdout,
                    "Model: %s\nSerial#: %s\nUptime: %" SCNu64
                    "s\nVoltage: %fV\nCurrent: %fmA\nTemperature: %f°C\nMode: %c\nSDA: %d\nSCL: %d\nSpeed: %dkHz\nSDA "
                    "pullup: %skΩ\nSCL pullup: %skΩ\nCCITT CRC: %x\n",
                    model, serial, uptime, voltage_v, current_ma, temp_celsius, mode, sda, scl, speed,
                    decode_pullup(pullups), decode_pullup(pullups >> 3), ccitt_crc);
            }
        }
    }

    if (options[CAPTURE])
    {
        i2cd.action("capturing I2C events");
        uint8_t data;
        maybeSet('c');
        auto stop = micros() + 1000000 * strtol(options[CAPTURE].last()->arg, nullptr, 10);
        int flushcount = 0;
        while (micros() < stop)
        {
            if (i2cd.read(&data, 1, 0, -1, 100) != 1)
                break; // We break even on EWOULDBLOCK, because idle tokens should always come
            decodeCapture(data);
            if (++flushcount == 10)
            {
                fflush(stdout);
                flushcount = 0;
            }
        }

        fprintf(stdout, "%s\n", color.DEFAULT);
    }

    if (options[DEV])
    {
        fprintf(stdout, "--dev not implemented, yet! Sorry :~-(\n");
        return 1;
    }

    maybeSet(monitor);

    if (i2cd.hasError())
    {
        fprintf(stderr, "%s\n", i2cd.error());
        return 1;
    }

    return 0;
}

/******************************************************************************************
 * The code below was taken and adapted from i2ctransfer.c from the package i2c-tools-4.3
 *****************************************************************************************/

enum parse_state
{
    PARSE_GET_DESC,
    PARSE_GET_DATA,
};

bool parse_transfer(int argc, const char* argv[], struct i2c_msg (&msgs)[I2C_RDWR_IOCTL_MAX_MSGS], int& nmsgs)
{
    int address = -1;
    int arg_idx = 0;
    nmsgs = 0;
    int i;

    // struct i2c_msg msgs[I2C_RDWR_IOCTL_MAX_MSGS];
    enum parse_state state = PARSE_GET_DESC;
    unsigned buf_idx = 0;

    for (i = 0; i < I2C_RDWR_IOCTL_MAX_MSGS; i++)
    {
        free(msgs[i].buf);
        msgs[i].buf = NULL;
    }

    while (arg_idx < argc)
    {
        const char* arg_ptr = argv[arg_idx];
        unsigned long len;
        unsigned long raw_data;
        __u16 flags;
        __u8 data;
        __u8* buf;
        char* end;

        if (nmsgs > I2C_RDRW_IOCTL_MAX_MSGS)
        {
            fprintf(stderr, "Error: Too many messages for --transfer (max: %d)\n", I2C_RDRW_IOCTL_MAX_MSGS);
            return false;
        }

        switch (state)
        {
            case PARSE_GET_DESC:
                flags = 0;

                switch (*arg_ptr++)
                {
                    case 'r':
                        flags |= I2C_M_RD;
                        break;
                    case 'w':
                        break;
                    default:
                        fprintf(stderr, "Error: Invalid direction in argument \"%s\"\n", argv[arg_idx]);
                        return false;
                }

                if (*arg_ptr == '?')
                {
                    if (!(flags & I2C_M_RD))
                    {
                        fprintf(stderr, "Error: variable length not allowed with write in argument \"%s\"\n",
                                argv[arg_idx]);
                        return false;
                    }
                    len = 256; /* SMBUS3_MAX_BLOCK_LEN + 1 */
                    flags |= I2C_M_RECV_LEN;
                    arg_ptr++;
                }
                else
                {
                    len = strtoul(arg_ptr, &end, 0);
                    if (len > 0xffff || arg_ptr == end)
                    {
                        fprintf(stderr, "Error: Length invalid in argument \"%s\"\n", argv[arg_idx]);
                        return false;
                    }
                    arg_ptr = end;
                }

                if (*arg_ptr)
                {
                    if (*arg_ptr++ != '@')
                    {
                        fprintf(stderr, "Error: Unknown separator after length int argument \"%s\"\n", argv[arg_idx]);
                        return false;
                    }

                    /* We skip 10-bit support for now. If we want it,
                     * it should be marked with a 't' flag before
                     * the address here.
                     */

                    address = Arg::Int7(arg_ptr);
                    if (address < 0)
                    {
                        fprintf(stderr, "Error: Not a valid I2C address \"%s\"\n", argv[arg_idx]);
                        return false;
                    }
                }
                else
                {
                    /* Reuse last address if possible */
                    if (address < 0)
                    {
                        fprintf(stderr, "Error: Missing address in --transfer string\n");
                        return false;
                    }
                }

                msgs[nmsgs].addr = address;
                msgs[nmsgs].flags = flags;
                msgs[nmsgs].len = len;

                // if (len)
                //{
                buf = (__u8*)malloc(len);
                if (!buf)
                {
                    fprintf(stderr, "Error: Not enough memory for --transfer buffer\n");
                    return false;
                }
                memset(buf, 0, len);
                msgs[nmsgs].buf = buf;
                if (flags & I2C_M_RECV_LEN)
                    buf[0] = 1; /* number of extra bytes */
                //}

                if (flags & I2C_M_RD || len == 0)
                {
                    nmsgs++;
                }
                else
                {
                    buf_idx = 0;
                    state = PARSE_GET_DATA;
                }

                break;

            case PARSE_GET_DATA:
                raw_data = strtoul(arg_ptr, &end, 0);
                if (raw_data > 0xff || arg_ptr == end)
                {
                    fprintf(stderr, "Error: Invalid data byte in argument \"%s\"\n", argv[arg_idx]);
                    return false;
                }
                data = raw_data;
                len = msgs[nmsgs].len;

                while (buf_idx < len)
                {
                    msgs[nmsgs].buf[buf_idx++] = data;

                    if (!*end)
                        break;

                    switch (*end)
                    {
                        /* Pseudo randomness (8 bit AXR with a=13 and b=27) */
                        case 'p':
                            data = (data ^ 27) + 13;
                            data = (data << 1) | (data >> 7);
                            break;
                        case '+':
                            data++;
                            break;
                        case '-':
                            data--;
                            break;
                        case '=':
                            break;
                        default:
                            fprintf(stderr, "Error: Invalid data byte suffix in argument \"%s\"\n", argv[arg_idx]);
                            return false;
                    }
                }

                if (buf_idx == len)
                {
                    nmsgs++;
                    state = PARSE_GET_DESC;
                }

                break;

            default:
                /* Should never happen */
                fprintf(stderr, "Internal Error: Unknown state in state machine!\n");
                return false;
        }

        arg_idx++;
    }

    if (state != PARSE_GET_DESC || nmsgs == 0)
    {
        fprintf(stderr, "Error: Incomplete --transfer string\n");
        return false;
    }

    return true;
}