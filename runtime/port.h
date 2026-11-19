/* port.h: what this port tells the AntiStatic runtime about Test Drive Off-Road 3 (from the
   toolkit's runtime/win32/port_example.h). Values only: nothing of the game's code or data. */
#pragma once

#define PORT_TITLE "Test Drive Off-Road 3"
#define PORT_SLUG "tdor3-native"
#define PORT_ENV "TDOR3"                      /* TDOR3_KEYS, TDOR3_PAD, ... as before */

/* the game's folder as the program sees it, and its program */
#define PORT_VROOT "C:\\TDOR3"
#define PORT_START_DIR ""
#define PORT_EXE "TDOR3.exe"
#define PORT_CMDLINE_ARGS ""
#define PORT_VOLUME_LABEL "SYSTEM"

/* above sr.dll and its plug-ins (0x10000000-0x10650000; DESIGN.md decision 3) */
#define PORT_ARENA_START 0x10800000u

/* the CD: drive D:, <game>/CD or --cd DIR, the label the game checks */
#define PORT_CD_LABEL "TDOR3"

/* the game opens a 640x480 screen; the runtime draws it at any size */
#define PORT_SCREEN_W 640
#define PORT_SCREEN_H 480

/* --test runs: the C++ line's thread turn order (this port's baselines) */
#define PORT_TEST_SCHEDULER 2

/* the machine: a Pentium II without MMX (port.c's cpu_cpuid) and RDTSC at 400 MHz (SurRender's
   timer calibration is seeded with it, port.c) */
#define PORT_CPU_ID_VERSION 0x0652u
#define PORT_CPU_ID_FEATURES 0x8131u
#define PORT_TSC_HZ 400000000.0
/* one processor, as the old runtime answered: SurRender's timer adds 0x2000 to the CPU version it
   keeps when there are more (sr.dll 0x100693b0), and the seeded calibration must match it */
#define PORT_CPU_COUNT 1
/* the joystick the game sees: two axes it binds (X, Y) and the right stick, eight buttons and
   the hat (port.c's port_pad_send fills them) */
#define PORT_PAD_AXES 4
#define PORT_PAD_BUTTONS 8
