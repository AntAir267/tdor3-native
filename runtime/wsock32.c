/* WSOCK32 (by ordinal): the game's network layer ("Demon") starts Winsock and makes a socket
   at start-up. First stage: Winsock starts, socket creation fails (network play is for later,
   DESIGN.md), so the game carries on without it. */
#include <string.h>
#include "runtime.h"

#define WSAENETDOWN 10050

static uint32_t wsa_error;

API(WSOCK32, 115, 2)                     /* WSAStartup(version, WSADATA*) */
{
    uint32_t d = ARG(1);
    if (d) {
        memset(gptr(d), 0, 400);
        W16(d, 0x0101);                  /* wVersion */
        W16(d + 2, 0x0202);              /* wHighVersion */
        gstrncpy(d + 4, "WinSock 2.0", 257);
        gstrncpy(d + 261, "Running", 129);
        W16(d + 390, 32767);             /* iMaxSockets */
        W16(d + 392, 65467);             /* iMaxUdpDg */
    }
    LOG(LOG_API, "WSAStartup(%x)", ARG(0));
    return 0;
}

API(WSOCK32, 111, 0) { return wsa_error; }             /* WSAGetLastError */

API(WSOCK32, 23, 3)                      /* socket(af, type, protocol) */
{
    LOG(LOG_API, "socket(%u, %u, %u): network play is not available yet", ARG(0), ARG(1), ARG(2));
    wsa_error = WSAENETDOWN;
    return 0xffffffffu;
}

API(WSOCK32, 3, 1) { return 0; }                       /* closesocket */
API(WSOCK32, 9, 1) { return ((ARG(0) & 0xff) << 8) | ((ARG(0) >> 8) & 0xff); }   /* htons */
API(WSOCK32, 2, 3) { wsa_error = WSAENETDOWN; return 0xffffffffu; }   /* bind */
API(WSOCK32, 13, 2) { wsa_error = WSAENETDOWN; return 0xffffffffu; }  /* listen */
API(WSOCK32, 1, 3) { wsa_error = WSAENETDOWN; return 0xffffffffu; }   /* accept */
API(WSOCK32, 16, 4) { wsa_error = WSAENETDOWN; return 0xffffffffu; }  /* recv */
API(WSOCK32, 19, 4) { wsa_error = WSAENETDOWN; return 0xffffffffu; }  /* send */
API(WSOCK32, 18, 5) { wsa_error = WSAENETDOWN; return 0xffffffffu; }  /* select */

API(WSOCK32, 57, 2)                      /* gethostname(buf, len) */
{
    if (ARG(1) < 6) { wsa_error = 10014; return 0xffffffffu; }
    gstrncpy(ARG(0), "TDOR3", ARG(1));
    return 0;
}

API(WSOCK32, 52, 1)                      /* gethostbyname(name): this machine, 127.0.0.1 */
{
    static uint32_t he;
    if (!he) {
        he = galloc(64);
        uint32_t name = he + 16, addr = he + 32, list = he + 40, alias = he + 48;
        gstrncpy(name, "TDOR3", 16);
        W32(addr, 0x0100007fu);
        W32(list, addr);
        W32(list + 4, 0);
        W32(alias, 0);
        W32(he, name);
        W32(he + 4, alias);
        W16(he + 8, 2);                  /* AF_INET */
        W16(he + 10, 4);
        W32(he + 12, list);
    }
    return he;
}
