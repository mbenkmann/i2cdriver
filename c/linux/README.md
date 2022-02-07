i2cdriver 1 "Feb 2022" i2cdriver "User Manual"
==================================================

# NAME
i2cdriver - universal control program for the I²Cdriver device

# SYNOPSIS
i2cdriver [options]

# BUILDING and INSTALLATION
see end of this document

# DESCRIPTION
The i2cdriver program aims to be the swiss army knife of command line tools to control
and use the I²Cdriver device. It offers the following major features:

### Activate USB Low Latency mode
Improves throughput of transfers. Requires root permissions. Once set this persists until
the I²Cdriver is unplugged or the computer is powered down.

### Configure I²Cdriver
Enable/Disable pullup resistors, set the clock rate, set monitor mode,...

### Capture I²C Events
Capture mode snoops the I²C bus and prints all observed events to stdout.
Output uses ANSI colors to make the display easier to read. The colors used are
similar to those used by the I²Cdriver device's physical display for monitor mode.

### Emulate a Linux /dev/i2c-* device
This allows the use of programs written for the Linux i2c API, such as stm32flash to
be used with the I²Cdriver device.

# OPTIONS
Long options can be abbreviated to a unique prefix.

`--help`               Print usage and exit.

`-v`   
`--verbose`            Verbose output.


`-d[<name>]`   
`--dev[=<name>]`       (requires /dev/cuse permissions) create /dev/`<name>` to emulate a
                     /dev/i2c-...  bus device.

`-b`, `--background`     Handle --dev in the background.

`-t <ttypath>`   
`--tty=<ttypath>`      Path to the ttyUSB device.
                     Not required if there is only 1 possibility.

`-p <kOhm>`   
`--pullups=<kOhm>`     Set pullups for SCL and SDA.
                     Values are 0, 1.1, 1.5, 2.2, 4.3, 4.7 .

`-k 100|400`  
`--kHz=100|400`        Set baud rate to 100kHz or 400kHz.

`--ll`                 Set USB latency to minimum (i.e. 1ms).

`--reset`              Attempt to unblock confused I2C devices to free up the I2C bus.

`--reboot`             Reboot the I2CDriver hardware. Takes half a second.

`-i`, `--info`           Print out the current status and configuration of the I2CDriver.

`-s`, `--scan`           Try to read from all I2C addresses
                     and print out addresses on which a device replies.

`-m`, `--monitor`        Switch the I2CDriver to monitor mode after all transmissions
                     and capturing are done.

`-c <secs>`   
`--capture=<secs>`     After all transmissions, capture events for `<secs>` seconds
                     and decode them to stdout.

`-x <data>`  
`--xfer=<data>`        Perform I2C transfer(s) according to `<data>`. See below for details.

`--pec`                Attach a Packet Error Checking byte to each subsequent
                     write-only `--xfer` datastream. Report PEC for read and write transfers.

# TRANSFER DATA STRING
A transfer may consist of multiple messages and is started with a START condition and ends with a STOP condition. Messages within the transfer are concatenated using a REPEATED START condition.

Messages and message data within the transfer data string are separated by whitespace or ','.

Each message begins with a descriptor of the following format:

`{r|w}<length_of_message>[@address]`   Example: `r3@0x50`


 `{r|w}`               'r' indicates a read (device->PC); 'w' indicates a write (PC->device).  
`<length_of_message>`  Number of bytes to read/write (0-65535).  
  `?`                 '?' as length for a read message causes 1 byte to be read   
                     and that byte determines how many more bytes will be read.   
`@address`             Address of the target device. Only required for the first message.
                     If omitted, the address from the prior message will be reused.

If the I2C message is a write, then a data block specifying the required number of bytes must follow the descriptor. It consists of `<length_of_message>` numbers, separated by
whitespace or ','.  
To make it easier to create larger data blocks, a number may have a suffix:  

`=`      keep value constant until end of message (i.e. `0=` means `0, 0, 0, ...`)

`+`      increase value by 1 until end of message (i.e. `0+` means `0, 1, 2, ...`)

`-`      decrease value by 1 until end of message (i.e. `0xff-` means `0xff, 0xfe, 0xfd, ...`)

`p`      use value as seed for an 8 bit pseudo random sequence
(i.e. `0p` means `0x00, 0x50, 0xb0, ...`)


# EXAMPLES
```
i2cdriver --kHz=100 --pullups=0 --ll --tty=/dev/ttyUSB0 --info

i2cdriver --capture=1000

i2cdriver --ll --tty=/dev/ttyUSB0 --kHz=400 --pullups=4.7 --dev=i2c-22

i2cdriver --xfer="w2@0x50 0x12 0x34, r2"
i2cdriver --xfer=w2@80,18,52,r2
i2cdriver --xfer="r?@0x77"
i2cdriver --xfer="w1024@0x77 0p"
```

# BUILDING AND INSTALLATION

### Requirements
On Debian/Ubuntu-based systems to build the main program

`apt install build-essential libfuse3-dev`

To build the manpage (optional)

`apt install go-md2man`

### Building
`cd i2cdriver/c`  
`make -f linux/Makefile`  

### Installing
To install under `/usr/local`:  

`make -f linux/Makefile install`  


To install under `/usr`:  

`make -f linux/Makefile DESTDIR=/usr install`

### Example udev rule
If the device created by `--dev` is supposed to be used by an unprivileged user, it is
useful to create a udev rule like the following which makes the device `i2c-22` available to user `doofus` and group `doofus` automatically whenever it is created.
That way you do not manually have to adjust ownership and/or permissions whenever you
use the `--dev` option.

```
SUBSYSTEM=="cuse", KERNEL=="i2c-22", OWNER="doofus", GROUP="doofus", MODE="0660"
```

Note that this does not change the permissions of the `/dev/cuse` device, access to which is required to use the `--dev` option.


# BUGS
At the time of this writing, output from --capture does not show all START conditions as
"S" symbol. This is a bug in the i2cdriver device firmware that causes it to not report
the START condition sometimes.

# SEE ALSO
 i2ctransfer(8),i2cdetect(8),i2cdump(8),i2cget(8),i2cset(8)
 