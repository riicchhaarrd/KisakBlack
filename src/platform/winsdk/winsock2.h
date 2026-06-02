// winsock2.h — minimal Winsock→BSD-sockets shim for non-Windows builds.
//
// The networking code (DemonWare/, qcommon net) is written against Winsock. Map the
// small surface it uses onto POSIX/BSD sockets. Grown as the net subsystem is
// ported; today it provides the socket address types + the common call aliases.
#ifndef KISAK_WINSOCK2_H
#define KISAK_WINSOCK2_H

#include "windows.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

typedef int            SOCKET;
typedef struct sockaddr SOCKADDR;
typedef int            socklen_t_win;

#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)

static inline int closesocket(int s)            { return close(s); }
static inline int WSAGetLastError()             { return errno; }
static inline int WSAStartup(WORD, void *)      { return 0; }
static inline int WSACleanup()                  { return 0; }

#endif // KISAK_WINSOCK2_H
