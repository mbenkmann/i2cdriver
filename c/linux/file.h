/*
 * Copyright (C) 2022 Matthias S. Benkmann
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software, to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following
 * conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef FILE_H
#define FILE_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

// Simple wrapper around a file descriptor to make UNIX syscalls easier to use.
class File
{
    // Filesystem path corresponding to the file descriptor.
    // Note that this correspondence is not guaranteed. You can construct a
    // File with an arbitrary path and file descriptor independently.
    const char* fpath;

    // Title used in error messages. This is usually an action like "opening file"
    // that will be inserted into an error message such as "Error opening file /bla".
    const char* tit;

    // If err != 0, this buffer contains the corresponding textual error message.
    char errmsg[256];

    // errno value of the error that "broke" the File. Once this is non-0 further
    // operations will be no-ops until clearError() is called. This allows
    // writing more readable code that does a few operations on a File and then
    // handles a potential error after those operations, rather than having to
    // intersperse error handling with file operations.
    int err;

    // The file descriptor all operations are performed on.
    int fd;

    // Set to true when EOF is encountered on a read() operation.
    bool eof;

    // Set to true if this File's open() function is used.
    bool close_on_destruction;

    // Initializes err and errmsg to 0 if retval >= 0 and based on errno if
    // retval < 0.
    // Returns true iff retval >= 0.
    bool checkError(int retval)
    {
        if (retval < 0)
        {
            err = errno;
            snprintf(errmsg, sizeof(errmsg), "Error %s %s: %s", tit, fpath, strerror(errno));
            return false;
        }
        else
        {
            err = 0;
            errmsg[0] = 0;
            return true;
        }
    }

    File(const File&);
    File& operator=(const File&);

  public:
    // Constructs a file from a path and optional file descriptor. If an already open file descriptor
    // is provided the path need not actually refer to a filesystem object.
    // Conversely, some operations (e.g. socket connections) use a path that is not in the filesystem.
    //
    // NOTE: The passed file descriptor is NOT closed on destruction of the File.
    //       Compare open().
    //
    // NOTE: The pointer fpath is used directly. DO NOT FREE!
    File(const char* _fpath, int filedes = -1)
        : fpath(_fpath), tit(""), err(0), fd(filedes), eof(false), close_on_destruction(false)
    {
        errmsg[0] = 0;
    }

    ~File()
    {
        if (close_on_destruction)
            ::close(fd);
    }

    // (Re)initializes the File as the constructor with the same signature.
    // ATTENTION! Does NOT close the previous file descriptor if the File is already open.
    void init(const char* _fpath, int filedes = -1)
    {
        fpath = _fpath;
        tit = "";
        err = 0;
        fd = filedes;
        eof = false;
        close_on_destruction = false;
        errmsg[0] = 0;
    }

    // Sets a title that will be included in potential error messages.
    // Example a title "opening file" will produce an error message like
    // "Error opening file /foo/bar: No such file or directory".
    void action(const char* titl) { tit = titl; }

    // Returns a message describing the most recent error. If there was no error, this is
    // the empty string "".
    // NOTE: Once an error occurs, further actions on the file will immediately
    // return false. This means you can call a series of functions and check
    // the error in the end and the error message will correspond to the error
    // that stopped the processing.
    const char* error() { return errmsg; }

    // Returns the errno error code of the most recent error. See error() for more info.
    // If no error is pending, returns 0.
    int errNo() { return err; }

    // Returns true iff a read operation encountered the end of file, meaning further
    // read operations will not produce any data. Note that this is not an error condition,
    // so errNo() will be 0.
    bool EndOfFile() { return eof; }

    // Returns true if this File is not open.
    // NOTE: A file descriptor passed into the constructor is assumed to be open.
    bool isClosed() { return fd < 0; }

    // Returns the file descriptor of this file if it is open, otherwise < 0.
    int fileDescriptor() { return fd; }

    // Clears a pending error so that future functions will not be skipped automatically.
    // This only makes sense for certain kinds of errors, e.g. EWOULDBLOCK, that leave
    // the file descriptor in a functioning state.
    // Also clears EndOfFile()
    void clearError()
    {
        checkError(0);
        eof = false;
    }

    // Returns true if the file is in an error state. See error().
    bool hasError() { return err != 0; }

    // Sets or clears the O_NONBLOCK flag.
    bool setNonBlock(bool onOff)
    {
        if (hasError())
            return false;

        int retval = fcntl(fd, F_GETFL);
        if (checkError(retval))
        {
            retval |= O_NONBLOCK;
            if (!onOff)
                retval = retval & ~O_NONBLOCK;
            checkError(fcntl(fd, F_SETFL, retval));
        }

        return err == 0;
    }

    // If opened, stats the file descriptor. If not opened, stats the file path.
    // Returns false if an error occurred.
    bool stat(struct stat* statbuf)
    {
        if (hasError())
            return false;

        int retval;
        if (fd >= 0)
            retval = ::fstat(fd, statbuf);
        else
            retval = ::stat(fpath, statbuf);

        return checkError(retval);
    }

    // Opens the file based on its path with the given flags. Closes the old
    // file descriptor first (regardless of whether it was passed to the
    // constructor or opened via open()). Unlike most other functions this function is
    // performed even if hasError(). You don't need to call clearError() first.
    // Returns: true on success, false in case of error
    // The mode argument is only used if flags includes something like O_CREAT.
    //
    // NOTE: If this function is used, the File will be automatically
    // closed on destruction.
    bool open(int flags = O_RDWR | O_NOCTTY | O_NONBLOCK, mode_t mode = 0666)
    {
        close();
        fd = ::open(fpath, flags, mode);
        close_on_destruction = true;
        return checkError(fd);
    }

    // Sets whether the File will close its file descriptor automatically on
    // destruction. This is automatically set if the File descriptor was
    // obtained via a call to this File's listen() or open(), but has to
    // be set manually if the file descriptor was passed into the constructor.
    void autoClose(bool on = true) { close_on_destruction = on; }

    // Closes the file. Returns true iff the syscall returned no error.
    // NOTE: If this File already hasError(), close() will attempt to
    // close the file descriptor but will not change errNo()/error().
    // This means if you check hasError() and print out error() after close()
    // you will get the most relevant error message.
    bool close()
    {
        eof = false;
        close_on_destruction = false;
        int _fd = fd;
        fd = -1;
        if (hasError())
        {
            ::close(_fd);
            return false;
        }
        return checkError(::close(_fd));
    }

    // Removes the file system entry referred to by the path passed to the constructor.
    // Does NOT close the file.
    // Returns true on success.
    bool unlink()
    {
        if (hasError())
            return false;
        int retval = ::unlink(fpath);
        return checkError(retval);
    }

    // If the file is not in error state, assigns newpath to this file as its new path,
    // THEN attempts to move the filesystem object from the old to the new path via rename(2).
    //
    // NOTE: The pointer newpath is used directly. DO NOT FREE!
    bool move(const char* newpath)
    {
        if (hasError())
            return false;
        const char* oldpath = fpath;
        fpath = newpath;
        int retval = ::rename(oldpath, newpath);
        return checkError(retval);
    }

    // Connects to a Unix Domain Socket on the file path passed to the constructor.
    // If the File is already open, it is closed first.
    // Another party must have used listen() and accept() on the same path
    // for a connection to be established.
    // Returns true iff the operation was successful. In that case you can
    // read from and write to the other party.
    //
    // NOTE: If this function is used, the File will be automatically
    // closed on destruction.
    bool connect()
    {
        if (hasError())
            return false;

        struct sockaddr_un addr;
        size_t fpath_len = strlen(fpath);
        if (fpath_len > sizeof(addr.sun_path) - 1) // -1 for 0 terminator
        {
            errno = ENAMETOOLONG;
            return checkError(-1);
        }
        close();

        int retval = socket(AF_UNIX, SOCK_STREAM, 0);
        if (checkError(retval))
        {
            fd = retval;
            close_on_destruction = true;
            addr.sun_family = AF_UNIX;
            memcpy(addr.sun_path, fpath, fpath_len);
            addr.sun_path[fpath_len] = 0;
            retval = ::connect(fd, (sockaddr*)&addr, sizeof(addr));
            checkError(retval);
        }

        return !hasError();
    }

    // If the file path passed to the constructor DOES NOT CONTAIN ':', binds a
    // Unix Domain Socket on the path.
    //
    // If the file path DOES CONTAIN ':', binds an IPv6 socket to the port
    // number following the last ':' in the name. If the string before that
    // last colon is "localhost", "127.0.0.1" or "::1", the socket will listen
    // only on the loopback interface. Otherwise it will listen on all interfaces
    // regardless of what the name is. NOTE: Support for name resolution and
    // interface determination may be implemented in the future. For compatibility
    // you should only use "127.0.0.1:<port>", "localhost:<port>", "::1:<port>"
    // and ":<port>".
    //
    // If the File is already open, it is closed first.
    // Returns true iff the operation was successful. In that case you can
    // use accept() to accept connections.
    //
    // backlog is the number of pending connections that can be queued. The
    // exact behaviour of this parameter is kernel-dependent.
    //
    // NOTE: If this function is used, the File will be automatically
    // closed on destruction.
    bool listen(int backlog = 16)
    {
        if (hasError())
            return false;

        struct sockaddr_un addr;
        size_t fpath_len = strlen(fpath);
        if (fpath_len > sizeof(addr.sun_path) - 1) // -1 for 0 terminator
        {
            errno = ENAMETOOLONG;
            return checkError(-1);
        }

        close();

        const char* colon = strrchr(fpath, ':');
        int retval;

        if (colon == 0)
            retval = socket(AF_UNIX, SOCK_STREAM, 0);
        else
            retval = socket(AF_INET6, SOCK_STREAM, 0);

        if (checkError(retval))
        {
            fd = retval;
            close_on_destruction = true;

            if (colon == 0)
            {
                addr.sun_family = AF_UNIX;
                memcpy(addr.sun_path, fpath, fpath_len);
                addr.sun_path[fpath_len] = 0;
                retval = bind(fd, (sockaddr*)&addr, sizeof(addr));
            }
            else
            {
                int colon_idx = colon - fpath;
                bool localhost = colon_idx > 0 &&
                                 (strncmp(fpath, "localhost", colon_idx) == 0 ||
                                  strncmp(fpath, "127.0.0.1", colon_idx) == 0 || strncmp(fpath, "::1", colon_idx) == 0);
                char* endptr;
                long port = strtol(colon + 1, &endptr, 10);
                if ((!localhost && colon_idx > 0) || colon[1] == 0 || port <= 0 || port > 65535 || *endptr != 0)
                {
                    errno = EADDRNOTAVAIL;
                    return checkError(-1);
                }

                struct sockaddr_in6 addr6;
                addr6.sin6_family = AF_INET6;
                addr6.sin6_port = htons(port);
                addr6.sin6_flowinfo = 0;

                if (localhost)
                    addr6.sin6_addr = IN6ADDR_LOOPBACK_INIT;
                else
                    addr6.sin6_addr = IN6ADDR_ANY_INIT;
                addr6.sin6_scope_id = 0;
                int on = 1;

                retval = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
                if (retval == 0)
                    retval = bind(fd, (sockaddr*)&addr6, sizeof(addr6));
            }

            if (checkError(retval))
                checkError(::listen(fd, backlog));
        }

        return !hasError();
    }

    // After listen() has been successfully called on this File, this function
    // can be used to accept a connection.
    // Returns a file descriptor for the accepted connection or -1 if an error
    // occurred.
    // NOTES:
    //   * EINTR is handled transparently and will never be returned as error.
    //   * If the socket is non-blocking and no connection is pending,
    //     the error condition EWOULDBLOCK is reported. Don't forget to call
    //     clearError() before calling accept() again.
    int accept()
    {
        if (hasError())
            return -1;
    eintr:
        int retval = ::accept(fd, 0, 0);
        if (retval < 0)
        {
            if (errno == EINTR)
                goto eintr;
            if (errno == EAGAIN) // translate EAGAIN to EWOULDBLOCK
                errno = EWOULDBLOCK;
        }

        checkError(retval);
        return retval;
    }

    // Assuming the file is open and a TTY device, this sets it up properly
    // for reading and writing.
    // Returns true on success and false on error.
    bool setupTTY(speed_t baudrate = B115200)
    {
        if (hasError())
            return false;

        struct termios tty;

        if (checkError(tcgetattr(fd, &tty)))
        {
            cfmakeraw(&tty);
            tty.c_cflag &= ~CSTOPB;
            tty.c_cflag &= ~CRTSCTS; // no hardware flow control

            tty.c_cc[VMIN] = 1;  // read blocks until at least 1 byte available
            tty.c_cc[VTIME] = 0; // no read timeout (i.e. blocking read)

            // I don't think the following 2 lines are necessary.
            // It seems to work without them. But they sound good.
            tty.c_cflag |= CREAD;  // enable receiver
            tty.c_cflag |= CLOCAL; // ignore modem control lines

            cfsetispeed(&tty, baudrate);
            cfsetospeed(&tty, baudrate);

            // Set up new attributes to become active after flush
            if (checkError(tcsetattr(fd, TCSADRAIN, &tty)))
            {
                // Flush (i.e. discard) all pending data in both directions
                checkError(tcflush(fd, TCIOFLUSH));
            }
        }

        return err == 0;
    }

    // Polls the file descriptor of this File for events.
    // See poll(2) for a description of timeout and return value.
    int poll(short events, int timeout_millis)
    {
        const int nfds = 1;
        pollfd fds[nfds];
        fds[0].fd = fd;
        fds[0].events = events;
        return ::poll(fds, nfds, timeout_millis);
    }

    // Writes n bytes from buf to the file. Unlike the system call write(2), this function
    // will only fail to write all bytes if a serious condition prevents it.
    // Note that if the file descriptor is set to O_NONBLOCK, both EAGAIN and EWOULDBLOCK
    // are treated as serious conditions and will cause a short write. In this case EAGAIN
    // will be converted to EWOULDBLOCK for purposes of error() and errNo(), so you only
    // have to deal with the code EWOULDBLOCK.
    // If bufrest and nrest are passed as non-null, they will be filled to refer to the
    // unwritten part of the buffer. nrest will be 0 iff all bytes were written.
    // It is allowed to pass &buf as bufrest. It is allowed to pass NULL for bufrest but
    // non-NULL for nrest or vice versa.
    // Returns true iff all bytes were written.
    bool writeAll(const void* buf, size_t n, size_t* nrest = 0, const void** bufrest = 0)
    {
        if (!hasError())
        {
            for (; n > 0;)
            {
                int retval = ::write(fd, buf, n);
                if (retval < 0)
                {
                    if (errno == EAGAIN)
                        errno = EWOULDBLOCK;

                    if (errno == EINTR)
                    {
                        continue;
                    }

                    checkError(retval);
                    break;
                }

                n -= retval;
                buf = (const char*)buf + retval;
            }
        }

        if (bufrest != 0)
            *bufrest = buf;
        if (nrest != 0)
            *nrest = n;

        return (!hasError());
    }

    // Reads up to bufsz bytes and stores them in buf.
    // more_wait: Whenever the file stops providing data in a manner that is not fatal
    //            (e.g. EAGAIN), the function will wait up to more_wait milliseconds for
    //            more data to become available before returning.
    //            more_wait <= 0 causes the function to end the first time data becomes
    //            unavailable (except EINTR, which is handled transparently)
    //            Note that more_wait only comes into play after the 1st byte has
    //            been read. The maximum time to wait for the 1st byte is governed
    //            by initial_wait and max_time (see below).
    // initial_wait: Maximum number of milliseconds to wait for the 1st byte if it
    //            is not already available. If < 0, the wait depends on whether the
    //            file is in non-blocking mode. If it is, the wait time will be 0 (i.e.
    //            the function will return immediately with an EWOULDBLOCK error if
    //            no byte is immediately available). If the file is blocking,
    //            the max wait time for the 1st byte will be max_time (see below).
    //            If initial_wait > max_time, initial_wait will be reduced to max_time.
    // max_time: If > 0, this is the maximum number of milliseconds the function
    //             will take, regardless if waiting or reading.
    //             If initial_wait<0 and the file descriptor is blocking, the function will
    //             wait up to this time for a first byte of data (because more_data
    //             only kicks in after the 1st byte has been received).
    //           If < 0 the function will continue reading until either more_wait
    //             or EOF or an error stops it. If the file is non-blocking, initial_wait < 0
    //             and no data is immediately available, this will be reported as an
    //             EWOULDBLOCK error.
    //           If == 0 the function will read all data that is immediately
    //             available. If no data is immediately available, an EWOULDBLOCK
    //             error condition is returned REGARDLESS of the non-blocking state
    //             of the file. This allows you to effectively do a non-blocking
    //             read on a blocking file.
    //
    // Returns: < 0 if error; otherwise the number of bytes read.
    //          EAGAIN is converted to EWOULDBLOCK so you only have to check for
    //          the latter. Note that this function reports EWOULDBLOCK as error
    //          condition only if 0 bytes have been read. If at least 1 byte has
    //          been read and reading stops on EWOULDBLOCK, the function returns
    //          with no error.
    //          If bufsz !=0 a return value of 0 indicates EOF.
    //
    // max_time is a rough guidance intended to prevent blocking indefinitely.
    // It is not a precise timing device.
    int read(void* buf, size_t bufsz, int more_wait = 0, int max_time = -1, int initial_wait = -1)
    {
        return tail(buf, bufsz, initial_wait, more_wait, max_time, true, false);
    }

    // Reads until all data has been read (within the time limits) and stores
    // the last bytes read in buf (up to bufsz bytes).
    // more_wait: Whenever the file stops providing data in a manner that is not fatal
    //            (e.g. EAGAIN), the function will wait up to more_wait milliseconds for
    //            more data to become available before returning.
    //            more_wait <= 0 causes the function to end the first time data becomes
    //            unavailable (except EINTR, which is handled transparently)
    //            Note that more_wait only comes into play after the 1st byte has
    //            been read. The max time to wait for the 1st byte is governed by
    //            initial_wait and max_time (see below).
    // initial_wait: Maximum number of milliseconds to wait for the 1st byte if it
    //            is not already available. If < 0, the wait depends on whether the
    //            file is in non-blocking mode. If it is, the wait time will be 0 (i.e.
    //            the function will return immediately if no byte is immediately
    //            available). If the file is blocking, the max wait time for the 1st
    //            byte will be max_time (see below).
    //            If initial_wait > max_time, initial_wait will be reduced to max_time.
    // max_time: If > 0, this is the maximum number of milliseconds the function
    //             will take, regardless if waiting or reading.
    //             If initial_wait<0 and the file descriptor is blocking, the function will
    //             wait up to this time for a first byte of data (because more_data
    //             only kicks in after the 1st byte has been received).
    //           If < 0 the function will continue reading until either more_wait
    //             or EOF or an error stops it. If the file is non-blocking, initial_wait < 0
    //             and no data is immediately available, the function returns immediately.
    //           If == 0 the function will read all data that is immediately
    //             available. If no data is immediately available, this function
    //             returns 0 REGARDLESS of the non-blocking state
    //             of the file. This allows you to effectively do a non-blocking
    //             read on a blocking file.
    //
    // Returns: < 0 if error; otherwise the number of bytes in the buffer. If the
    //          returned number is < bufsz, this corresponds to the total bytes read.
    //          If the number is bufsz, it is possible that more bytes have been read.
    //          This function DOES NOT report EAGAIN or EWOULDBLOCK as error
    //          conditions, even if nothing has been read.
    //
    // max_time is a rough guidance intended to prevent blocking indefinitely.
    // It is not a precise timing device.
    int tail(void* buf, size_t bufsz, int more_wait = 0, int max_time = -1, int initial_wait = -1)
    {
        return tail(buf, bufsz, initial_wait, more_wait, max_time, false, true);
    }

    // Tries to create a new directory dir with given mode. Returns null if creation
    // failed and errno is set. Note in particular that creation will fail if there
    // is already an existing directory.
    // If dir ends in one or more '?' characters, these will be replaced with
    // numbers starting from 0 and increasing until all '?' are 9 or a directory
    // could be created.
    // If creation succeeds and dir contains no '?', the function returns dir.
    // If creation succeeds and dir contains '?', the function returns the name
    // of the created directory, alloc'd with malloc(). Don't forget to free() it
    // if necessary.
    static const char* createDirectory(const char* dir, mode_t mode)
    {
        void* tofree = 0;
        int a = 0;
        int b = 0;
        int len = strlen(dir);
        char* dir2 = 0;

        if (len > 0 && dir[len - 1] == '?')
        {
            dir2 = strdup(dir);
            dir = dir2;
            tofree = (void*)dir;
            b = len;
            for (a = b; a > 0; dir2[--a] = '0')
                if (dir[a - 1] != '?')
                    break;

            // dir[a] is the 1st '0'
            // dir[b] == 0
        }

        for (;;)
        {
            int retval = mkdir(dir, mode);
            if (retval == 0)
                return dir;

            int i = b - 1;
            while (i >= a)
            {
                dir2[i]++;
                if (dir2[i] <= '9')
                    break;
                dir2[i] = '0';
                --i;
            }

            if (i < a)
                break;
        }

        int errno_saved = errno;
        free(tofree);
        errno = errno_saved;
        return 0;
    }

    // Tries to create a new file fpath with given mode. Returns null if creation
    // failed and errno is set. Note in particular that creation will fail if there
    // is already an existing file.
    // If fpath ends in one or more '?' characters, these will be replaced with
    // numbers starting from 0 and increasing until all '?' are 9 or a file
    // could be created.
    // If creation succeeds and fpath contains no '?', the function returns fpath.
    // If creation succeeds and fpath contains '?', the function returns the name
    // of the created file, alloc'd with malloc(). Don't forget to free() it
    // if necessary.
    static const char* createFile(const char* fpath, mode_t mode)
    {
        void* tofree = 0;
        int a = 0;
        int b = 0;
        int len = strlen(fpath);
        char* fpath2 = 0;

        if (len > 0 && fpath[len - 1] == '?')
        {
            fpath2 = strdup(fpath);
            fpath = fpath2;
            tofree = (void*)fpath;
            b = len;
            for (a = b; a > 0; fpath2[--a] = '0')
                if (fpath[a - 1] != '?')
                    break;

            // fpath[a] is the 1st '0'
            // fpath[b] == 0
        }

        for (;;)
        {
            int retval = ::open(fpath, O_CREAT | O_EXCL | O_RDWR, mode);
            if (retval >= 0)
            {
                ::close(retval);
                return fpath;
            }

            int i = b - 1;
            while (i >= a)
            {
                fpath2[i]++;
                if (fpath2[i] <= '9')
                    break;
                fpath2[i] = '0';
                --i;
            }

            if (i < a)
                break;
        }

        int errno_saved = errno;
        free(tofree);
        errno = errno_saved;
        return 0;
    }

  private:
    // Combines read() and tail() through use of report_ewouldblock and do_tail
    int tail(void* buf, size_t bufsz, int initial_wait, int more_wait, int max_time, bool report_ewouldblock,
             bool do_tail)
    {
        if (hasError())
            return -1;

        if (bufsz == 0)
            return 0;

        struct timeval tv
        {
            0, 0
        };
        if (max_time < 0)
            max_time = INT_MAX;
        gettimeofday(&tv, 0);
        int64_t start_millis = (int64_t)tv.tv_sec * 1000 + (tv.tv_usec + 500) / 1000;
        int64_t stop_millis = start_millis + max_time;

        if (more_wait < 0)
            more_wait = 0;

        const int nfds = 1;
        pollfd fds[nfds];
        fds[0].fd = fd;
        fds[0].events = POLLIN;

        int n = bufsz;
        int full_buffers = 0; // counts how often we completely filled the buffer

        void* bufstart = buf;
        int retval;

        if (initial_wait < 0)
        {
            initial_wait = max_time;
            retval = fcntl(fd, F_GETFL);
            if (!checkError(retval))
                return retval;
            if ((retval & O_NONBLOCK) != 0)
                initial_wait = 0;
        }
        else if (initial_wait > max_time)
            initial_wait = max_time;

    wait_for_data:
        retval = ::poll(fds, nfds, initial_wait);
        if (retval < 0 && errno == EINTR)
            goto wait_for_data;

        if (retval == 0)
        { // NOTE: On EOF, poll() always returns an event, so we don't ever get here on EOF.
            if (report_ewouldblock)
            {
                errno = EWOULDBLOCK;
                checkError(-1);
                return -1;
            }
            else
                return 0;
        }
        else if (retval < 0)
        {
            checkError(retval);
            return retval;
        }

        for (;;)
        {
            int poll_millis = max_time;
            if (more_wait < poll_millis)
                poll_millis = more_wait;

        wait_for_more_data:
            retval = ::poll(fds, nfds, poll_millis);
            if (retval < 0 && errno == EINTR)
                goto wait_for_more_data;

            // don't call read() if nothing is ready, in case the file descriptor
            // is blocking.
            if (retval == 0)
                break;

            do
                retval = ::read(fd, buf, n);
            while (retval < 0 && errno == EINTR);

            if (retval == 0)
            { // means EOF because n > 0
                eof = true;
                break;
            }
            else if (retval < 0)
            {
                // EAGAIN and EWOULDBLOCK are not possible here, because our
                // initial poll() before the main loop made sure we get at least EOF
                // or a byte.
                checkError(retval);
                return retval;
            }
            else
            {
                buf = (char*)buf + retval;
                n -= retval;
                if (n == 0) // buffer full
                {
                    ++full_buffers;

                    if (!do_tail)
                        break;

                    // If we're tailing, restart from start of buffer
                    n = bufsz;
                    buf = bufstart;
                }
            }

            gettimeofday(&tv, 0);
            int64_t millis = (int64_t)tv.tv_sec * 1000 + (tv.tv_usec + 500) / 1000;
            if (millis > stop_millis) // don't use >= because of max_time == 0
                break;
            else
                max_time = stop_millis - millis;
        }

        if (full_buffers == 0)
            return bufsz - n;

        // we need to swap the first part of the buffer with the 2nd part because
        // the first part was read later

        char* a = (char*)bufstart;
        char* b = (char*)buf;
        // [a  b]  <->  [b  b+n]

        if (a != b)
        {
            const int MOVE_BUF = 65536;
            char movebuf[MOVE_BUF];
            for (; n > 0;)
            {
                int moveSz = n;
                if (moveSz > MOVE_BUF)
                    moveSz = MOVE_BUF;
                memcpy(movebuf, b, moveSz);
                memmove(a + moveSz, a, b - a);
                memcpy(a, movebuf, moveSz);
                a += moveSz;
                b += moveSz;
                n -= moveSz;
            }
        }

        return bufsz;
    }

  public:
};

#endif
