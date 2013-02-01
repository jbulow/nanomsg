/*
    Copyright (c) 2012 250bpm s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "efd.h"

#if defined NN_HAVE_WINDOWS

#define NN_EFD_PORT 5907

#include "err.h"
#include "fast.h"

#include <string.h>

void nn_efd_init (struct nn_efd *self)
{
    SECURITY_ATTRIBUTES sa = {0};
    SECURITY_DESCRIPTOR sd;
    BOOL brc;
    HANDLE sync;
    DWORD dwrc;
    SOCKET listener;
    int rc;
    struct sockaddr_in addr;
    BOOL reuseaddr;
    BOOL nodelay;

    /*  Make the following critical section accessible to everyone. */
    sa.nLength = sizeof (sa);
    sa.bInheritHandle = FALSE;
    brc = InitializeSecurityDescriptor (&sd, SECURITY_DESCRIPTOR_REVISION);
    win_assert (brc);
    brc = SetSecurityDescriptorDacl(&sd, TRUE, (PACL) NULL, FALSE);
    win_assert (brc);
    sa.lpSecurityDescriptor = &sd;

    /*  This function has to be enclosed in a system-wide critical section
        so that two instances of the library don't accidentally create an efd
        crossing the process boundary. 
        CAUTION: This critical section has machine-wide scope. Thus, it must
        be properly exited even before crashing the process by an assertion. */
    sync = CreateEvent (&sa, FALSE, TRUE, "Global\\nanomsg-port-sync");
    win_assert (sync != NULL);

    /*  Enter the critical section. */
    DWORD dwrc = WaitForSingleObject (sync, INFINITE);
    nn_assert (dwrc == WAIT_OBJECT_0);

    /*  Unfortunately, on Windows the only way to send signal to a file
        descriptor (SOCKET) is to create a full-blown TCP connecting on top of
        the loopback interface. */
    self->w = INVALID_SOCKET;
    self->r = INVALID_SOCKET;

    /*  Create listening socket. */
    listener = socket (AF_INET, SOCK_STREAM, 0);
    if (nn_slow (listener == SOCKET_ERROR))
        goto wsafail;
    brc = SetHandleInformation ((HANDLE) listener, HANDLE_FLAG_INHERIT, 0);
    win_assert (brc);

    /*  This prevents subsequent attempts to create a signaler to fail bacause
        of "TCP port in use" problem. */
    reuseaddr = 1;
    rc = setsockopt (listener, SOL_SOCKET, SO_REUSEADDR,
        (char*) &reuseaddr, sizeof (reuseaddr));
    if (nn_slow (rc == SOCKET_ERROR))
        goto wsafail;

    /*  Bind the listening socket to the local port. */
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    addr.sin_port = htons (NN_EFD_PORT);
    rc = bind (listener, (const struct sockaddr*) &addr, sizeof (addr));
    if (nn_slow (rc == SOCKET_ERROR))
        goto wsafail;

    /*  Start listening for the incomming connections. In normal case we are
        going to accept just a single connection, so backlog buffer of size
        1 is sufficient. */
    rc = listen (listener, 1);
    if (nn_slow (rc == SOCKET_ERROR))
        goto wsafail;

    /*  Create the writer socket. */
    self->w = socket (AF_INET, SOCK_STREAM, 0);
    if (nn_slow (listener == SOCKET_ERROR))
        goto wsafail;
    brc = SetHandleInformation ((HANDLE) self->w, HANDLE_FLAG_INHERIT, 0);
    win_assert (brc);

    /*  Set TCP_NODELAY on the writer socket to make efd as fast as possible.
        There's only one byte going to be written, so batching would not make
        sense anyway. */
    nodelay = 1;
    rc = setsockopt (self->w, IPPROTO_TCP, TCP_NODELAY, (char*) &nodelay,
        sizeof (nodelay));
    if (nn_slow (rc == SOCKET_ERROR))
        goto wsafail;

    /*  Connect the writer socket to the listener socket. */
    rc = connect (self->w, (sockaddr*) &addr, sizeof (addr));
    if (nn_slow (rc == SOCKET_ERROR))
        goto wsafail;

    while (1) {

        /*  Accept new incoming connection. */
        self->w = accept (listener, (struct sockaddr*) &addr, sizeof (addr));
        if (nn_slow (self->w == INVALID_SOCKET))
            goto wsafail;

        /*  Check that the connection actually comes from the localhost. */
        if (nn_fast (addr.sin_addr.s_addr == htonl (INADDR_LOOPBACK)))
            break;

        /*  If not so, close the connection and try again. */
        rc = closesocket (self->w);
        if (nn_slow (rc == INVALID_SOCKET))
            goto wsafail;
    }

    /*  Listener socket can be closed now as no more connections for this efd
        are going to be established anyway. */
    rc = closesocket (listener);
    if (nn_slow (rc == INVALID_SOCKET))
        goto wsafail;

    /*  Leave the critical section. */
    brc = SetEvent (sync);
    win_assert (brc != 0);
    brc = CloseHandle (sync);
    win_assert (brc != 0);

    return;

wsafail:
    rc = nn_err_wsa_to_posix (WSAGetLastError ());
    brc = SetEvent (sync);
    win_assert (brc != 0);
    brc = CloseHandle (sync);
    win_assert (brc != 0);
    errno_assert (0, rc);
}

#elif defined NN_USE_SOCKETPAIR

#include "err.h"
#include "fast.h"

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

void nn_efd_init (struct nn_efd *self)
{
    int rc;
    int flags;
    int sp [2];

#if defined SOCK_CLOEXEC
    rc = socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp);
#else
    rc = socketpair (AF_UNIX, SOCK_STREAM, 0, sp);
#endif
    errno_assert (rc == 0);
    self->r = sp [0];
    self->w = sp [1];

#if !defined SOCK_CLOEXEC && defined FD_CLOEXEC
    rc = fcntl (self->r, F_SETFD, FD_CLOEXEC);
    errno_assert (rc != -1);
    rc = fcntl (self->w, F_SETFD, FD_CLOEXEC);
    errno_assert (rc != -1);
#endif

    flags = fcntl (self->r, F_GETFL, 0);
	if (flags == -1)
        flags = 0;
	rc = fcntl (self->r, F_SETFL, flags | O_NONBLOCK);
    errno_assert (rc != -1);
}

void nn_efd_term (struct nn_efd *self)
{
    int rc;

    rc = close (self->r);
    errno_assert (rc == 0);
    rc = close (self->w);
    errno_assert (rc == 0);
}

int nn_efd_getfd (struct nn_efd *self)
{
    return self->r;
}

void nn_efd_signal (struct nn_efd *self)
{
    ssize_t nbytes;
    char c = 101;

    nbytes = send (self->w, &c, 1, 0);
    errno_assert (nbytes != -1);
    nn_assert (nbytes == 1);
}

void nn_efd_unsignal (struct nn_efd *self)
{
    ssize_t nbytes;
    uint8_t buf [16];

    while (1) {
        nbytes = recv (self->r, buf, sizeof (buf), 0);
        errno_assert (nbytes >= 0);
        if (nn_fast (nbytes < sizeof (buf)))
            break;
    }
}


#elif defined NN_USE_EVENTFD

#include "err.h"

#include <stdint.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>

void nn_efd_init (struct nn_efd *self)
{
    int rc;
    int flags;

    self->efd = eventfd (0, EFD_CLOEXEC);
    errno_assert (self->efd != -1);

    flags = fcntl (self->efd, F_GETFL, 0);
	if (flags == -1)
        flags = 0;
	rc = fcntl (self->efd, F_SETFL, flags | O_NONBLOCK);
    errno_assert (rc != -1);
}

void nn_efd_term (struct nn_efd *self)
{
    int rc;

    rc = close (self->efd);
    errno_assert (rc == 0);
}

int nn_efd_getfd (struct nn_efd *self)
{
    return self->efd;
}

void nn_efd_signal (struct nn_efd *self)
{
    const uint64_t one = 1;
    ssize_t nbytes;

    nbytes = write (self->efd, &one, sizeof (one));
    errno_assert (nbytes == sizeof (one));
}

void nn_efd_unsignal (struct nn_efd *self)
{
    uint64_t count;

    /*  Extract all the signals from the eventfd. */
    ssize_t sz = read (self->efd, &count, sizeof (count));
    errno_assert (sz == sizeof (count));
    nn_assert (count > 0);
}

#endif
