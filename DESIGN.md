# Test Drive Off-Road 3 native: design notes

A native Linux port of Test Drive Off-Road 3 (Accolade / Infogrames, 1999, PC) by static
recompilation, the same method as 1NSANE: Recompiled (github.com/AntAir267/1nsane-recompiled): the game's own x86 code is
translated into C on the user's machine and built against a stand-in for Windows and DirectX on
SDL3 + Vulkan. Bring your own game: the repository holds only our translator and runtime, never
the game's code, data or the generated C.

Reference projects (the author's own): 1NSANE: Recompiled (the translator in tools/ is forked
from it; its runtime is the model for ours), a LEGO Racers 2 port (WRAPS/PATCHES overrides,
movies), and a TDOR3 file-format study (file formats, Ghidra decompiles of TDOR3.exe and sr.dll in
work/ghidra/decomp*, render-state notes in its README).

## Decisions

### 1. Goal: faithful by default, extras as options (as 1NSANE, 2026-10-24)
- Default is the game as shipped: same physics, AI, timing and look; original bugs kept except
  crashes.
- Targets (the user's brief): runs natively on Linux (KDE Plasma Wayland; the handheld's
  2560x1600 at 200% scale, often docked to 2560x1440 @ 165 Hz), any resolution with Hor+
  widescreen and HUD/menus scaled, correct timing at high refresh rates, full gamepad support
  (Xbox layout, analog triggers, hot-plug, button prompts), CD music from the user's OGG
  tracks, Bink movies.

### 2. The modules and what happens to each (2026-10-24)

Measured with tools/ and work/analysis/ (1NSANE's discovery + codegen run over every module):

| Module | Base | Code | Functions | Fate |
|---|---|---|---|---|
| TDOR3.exe | 0x400000 (no relocations) | 168K insns | 3,260 | translated |
| sr.dll (SurRender 3D core, 2,220 C++ exports) | 0x10000000 | 154K insns | 4,757 | translated, at its own base |
| srVP_Generic.dll (vector processor) | 0x10000000 | 5K | 179 | translated, rebased |
| srEXT_Unzip.dll (zip archives) | 0x10000000 | 22K | 183 | translated, rebased |
| srEXT_default.dll (image loaders) | 0x10000000 | 7.5K | 269 | translated, rebased |
| srEXT_BINK.dll (Bink textures/movies) | 0x10000000 | 1.5K | 120 | translated, rebased |
| MSVCRT.DLL (1999) | 0x78000000 | 66K | 1,325 | translated, at its own base |
| MSVCP60.DLL | 0x780c0000 | 65K | 3,451 | translated, at its own base |
| srDD_DirectX6.dll (and Glide2x/3x, Software, Software2) | | 18K | 195 | replaced: native driver |
| srVP_Pentium*/AMD3DNow | | | | not loaded (Generic is enough) |
| binkw32.dll | | 30K, MMX | 305 | replaced: FFmpeg (libavcodec has Bink) |

All of it translates with 1NSANE's code generator as is (about 1.5M lines of C); the only gaps
are a few MMX routines in sr.dll's colour-surface code (SurRender picks them by CPUID, so the
CPUID stand-in reports a CPU without MMX) and the `cpuid`/`pushfd` probes in the VP/driver DLLs.

Why this split:
- **The exe and sr.dll are one program.** TDOR3.exe imports 343 of sr.dll's C++ exports (scene
  graph, meshes, materials, cameras, textures, timers, streams) and the C++ objects cross the
  boundary both ways (vtables, `new` in one module and `delete` in the other). Replacing sr.dll
  would mean reimplementing SurRender; translating it is the same work as translating the exe.
- **The display driver is a clean seam.** sr.dll loads `srDD_<name>.dll` by name, checks its API
  version, and calls `srDDInitDevice(n)`, which returns an `srDD` C++ object with 43 virtual
  methods. sr.dll's own logging wrapper `srDebugDD` exports every one of them with its mangled
  signature, so the interface is known exactly (work/analysis/srdd_iface.txt): `getInfo`,
  `getWindowList`, `getTextureFormats`, `openWindow(OpenInfo, OpenResult)`, `closeWindow`,
  `beginFrame`/`endFrame`/`flushFrame`/`flipFrame`, `clearBuffers`, `bufferOp`, `setScissor`,
  `setViewPort`, `setClearValues`, `setFogColor`, `setShader(srShader)`, `setTextureParameters`,
  `bindTexture`/`preBindTexture`/`deleteTexture`/`texImage`/`texSubImage`, palettes,
  `createContext`/`deleteContext`, `getDriverInfo`, `fence`, `getBufferPixelFormat`,
  `setPolygonMode`, `setCullMode`, `setProjectionMatrix`, `setVertexArrayInfo`,
  `drawElements`, `drawArrays`, `setPolygonOffset`. It is OpenGL 1.x in shape. So instead of
  translating srDD_DirectX6 and rebuilding DirectDraw 6 + Direct3D 6 underneath it, we write a
  native SurRender driver on Vulkan (runtime/srdd.c + runtime/vk.c): no DirectX layer at all,
  any resolution directly in `getWindowList`/`openWindow`, and the projection, viewport and
  every draw in our hands for widescreen and scaling. The game picks drivers in tdor3.cfg's
  order (Glide3x, Glide2x, DirectX6, Software): LoadLibrary fails for the Glide ones and
  returns our built-in module for "srDD_DirectX6.dll", so no game code is patched for it.
  The struct layouts (OpenInfo, Texture, VertexArrayInfo, srShader...) come from srDD_DirectX6
  and srDD_Software's code and sr.dll's srShader printer (0x1003e0b0).
- **The C runtimes are translated, not reimplemented.** Every module shares one MSVCRT heap and
  one MSVCP60 (iostreams); they import data as well as code (`_iob`, `_pctype`, `std::cout`,
  iostream vtables), and sr.dll throws C++ exceptions (`_CxxThrowException`,
  `__CxxFrameHandler`). Translating the two DLLs keeps all of that exactly as on Windows and
  moves the stand-in down to KERNEL32 (heaps, files, TLS, threads), which 1NSANE's runtime
  already has. Hot routines (memcpy, memset, strlen...) can be swapped for host versions
  (HOOKS), as 1NSANE and LR2 do.
- **binkw32 is replaced** by host functions over FFmpeg (8 imports used by srEXT_BINK:
  BinkOpen, BinkDoFrame, BinkNextFrame, BinkCopyToBuffer, BinkGoto, BinkClose,
  BinkOpenDirectSound, BinkSetSoundSystem). The movies (CD/Movies/*.bik: Legals, Intro,
  Credits, World Cup, Win1-3, Lose1-3) can be skipped until then.

### 3. Translation (2026-10-24)
- Forked from 1NSANE's tools/ (pe.py, x86.py, discover.py, codegen.py, fpustack.py), extended
  to several modules: each DLL is rebased to its fixed address (its relocations applied at
  translation time and again by the runtime's loader), the modules are presented to discovery
  and codegen as one image, and an import that another translated module exports becomes a
  direct call to its translated function (data imports are read through the import table,
  which the runtime's loader fills as Windows would). DLL relocations also give exact code
  pointer candidates for discovery.
- Fixed layout: TDOR3.exe 0x00400000, sr.dll 0x10000000 (its own base, so addresses match the
  Ghidra project and the rip notes), the small sr DLLs from 0x10600000, MSVCRT 0x78000000,
  MSVCP60 0x780c0000; program memory (heaps, stacks, VirtualAlloc) 0x10800000-0x70000000;
  runtime thunks 0x7f000000.
- As 1NSANE: registers in a CPU record, the program's stack in program memory, x87 maths in
  80-bit long double with the program's control word, one host thread per program thread.
- Code generation runs on the user's machine from their own files (checked by SHA-256 in
  builds/tdor3_us.py).

### 4. Windows stand-in (2026-10-24)
- runtime/ forked from 1NSANE's: kernel32 (files, heaps, threads, TLS, sync), user32 (one SDL
  window, messages), advapi32 (registry in a settings file), ole32, winmm (timeGetTime, MCI CD
  audio from MUSIC/TrackNN.ogg, mixer volume), dinput (keyboard, mouse, joystick), dsound.
- Imports used (all modules): MSVCRT 122 distinct, MSVCP60 58, KERNEL32 65 + MSVCRT's 144,
  USER32 55, GDI32 9, ADVAPI32 21, WINMM 12, DINPUT 1, DSOUND 1, ole32 2, WSOCK32 13 (network
  play: later), VERSION 3, comdlg32 1.
- Exceptions (built 2026-10-25): KERNEL32 RaiseException walks the program's SEH chain
  (fs:[0]) and calls the translated handlers (MSVCRT's __CxxFrameHandler / _except_handler3),
  RtlUnwind calls them to unwind (runtime/eh.c). Going on after a handler is the one jump
  translation cannot make: into the middle of the function that caught (_JumpToContinuation
  0x7800ac4f for C++, hooked; a call of the __except block for SEH, caught in icall).
  tools/eh.py reads the exception tables (1174 FuncInfo, 41 SEH scope tables: 2460 handler
  funclets made entries, 198 places to go on at, keyed by the __ehhandler stub / scope table
  the owner's prologue pushes); codegen gives each owner (47 functions) a wrapper that pushes
  a frame record (program esp at entry, its places, a __builtin_setjmp buffer) around the body;
  the body jumps to the place (or tail-calls it where discovery made it a function of its own).
  eh_resume picks the youngest record holding the place whose frame is at or above the stack
  resumed with, sets esp/ebp and longjmps. Tested: a failed movie open (TDOR3_BINK_FAIL=1)
  throws srIOManager::Error, the game catches it and asks for the CD (its own handling).
  The SEH __except path is built the same way but has not been seen running.
- Files: the game folder is read only; the cfg, saves and logs live in
  ~/.local/share/tdor3-native/.

### 5. Graphics (2026-10-24)
- The native srDD driver turns SurRender's GL-like stream into Vulkan: textures and palettes as
  images, srShader states into pipeline keys, vertex arrays into ring buffers, drawElements /
  drawArrays into draws. Vertices arrive transformed by sr.dll's vector processor (to be
  confirmed: setProjectionMatrix's e_type says which space).
- Any resolution (done 2026-10-25): the game always opens 640x480 (0x421e81 asks
  getDisplayMode(640, 480, 16/24/32); it has no resolution menu). The renderer draws at the
  window's size in pixels (option resolution = native | original | WxH | H) and maps the
  game's 640x480 onto the middle 4:3 of it (vk.c apply_view); reads and writes of the back
  buffer go through a game-sized image. Widescreen (Hor+, option widescreen, on): every 3D
  view goes through srScene::render (0x1005ea20); a hook widens the camera's view plane (+0x140
  x0, +0x150 x1) by the target's aspect / (4/3) for that call and the renderer spreads that
  scene's viewport across the full width (vk_wide). 2D (menus, movies) stays 4:3 in the
  middle; a clear of the whole game screen clears the whole target. runtime/scene.c.
  SurRender batches geometry, so the hook flushes its renderers (srGERD::flushRenderers
  0x10023bd0) before and after the scene: 2D queued before it (the car select's stat labels
  and the pointer, queued before its turning truck) was otherwise drawn inside the widened
  scene, stretched sideways and without the 2D sampling below. Every 3D scene is marked
  (vk_wide) whether widened or not: 2D is everything else.
  The race HUD (option hud = edges | middle): its overlays (Overlay.cpp objects drawn by
  0x414cb6, list [0x4c4950]) are drawn one by one by runtime/hud.c, SurRender's batches flushed
  after each (srGERD::flushRenderers 0x10023bd0); srdd.c keeps an overlay's draws and places it
  by where it sits (left / right third: moved out to that screen edge at the same distance and
  size; else the middle): lap / checkpoint count, map left, gauges and lap times right; timer,
  place, pause message and results table centred.
- Full screen by default (display = window for a window of 80 % of the desktop, the screen's
  shape); SDL_WINDOW_HIGH_PIXEL_DENSITY so a 200 % desktop gets its real pixels.
- Anti-aliasing (option antialiasing = off | 2x | 4x, 4x with a window, off in tests):
  supersampling, the target drawn 1.41 or 2 times larger per side and filtered down at present
  (the trees are alpha-tested, which multisampling would leave jagged); bounded to 5120x3200.
  Anisotropic filtering (option anisotropic, 16) on mipmapped linear samplers.
- 2D sampling (draw.frag): Direct3D 6 sampled a pixel at its top-left corner, so 2D draws
  sample half a game pixel up-left (glyph cells end half a texel short; see Progress). Pieces
  that show a whole image (gauges, panels, menu tiles: texture coordinates within 0..1 that
  vary) are kept half a texel inside it: drawn larger than at 640x480 the filter otherwise
  reached round to the image's far side, a dark line along the speedometer's edges.
- Frame pacing (done 2026-10-25): physics runs in fixed 33 ms steps (30 Hz). runtime/smooth.c
  draws between the last two steps by accumulator / step: SurRender nodes changed in the race
  loop (srNode::setWSDirty 0x10059d90; ~srNode 0x100590a0 drops them) are blended (rotation
  +0x18, location +0x60, both doubles) while the frame's scenes draw (0x41e526 .. end of
  0x4221e0), then put back. Nodes changing between steps and jumps are left alone. Option
  smooth (on).

### 6. Input (2026-10-24; controller done 2026-10-25)
- DirectInput stand-in on SDL3 gamepads, keyboard and pad together. The game looks for
  joysticks once at start-up and knows X/Y, 8 buttons and the hat (binding codes decoded at
  0x457a1a: 0x200-0x203 X-/X+/Y-/Y+, 0x204+n button n, 0x20c+4*hat+dir). runtime/pad.c keeps one
  joystick connected from the start (hot-plug behind it): X left stick, Y = LT - RT (default
  THROTTLE Y-, BRAKE Y+, analogue), buttons 0 RB shift up, 1 LB shift down, 2 A e-brake,
  3 L3 horn, 4-7 B X Y R3, hat = D-pad or right stick (rear view, look left/right).
  Menus answer keys only: runtime/game.c types arrows / Enter / Escape from the pad outside a
  race. In a race the keyboard-only commands (the exe's accelerator table "TDOR3", 53 entries:
  Esc/P pause, F1-F5 cameras, I ignition, L lights, H horn, Ctrl+letters cheats/HUD...) get
  buttons: Start pause, View next camera, Y ignition, X lights; paused, View is Q (quit) and A
  Enter (race on).
- Button prompts: the race's own lines naming keys ("Press 'Q' to quit.", "Press 'Enter' to
  race.", "Press 'I' to start the engine." in .data at 0x4b48f4 / 0x4b4908 / 0x4b4564) are
  rewritten in place while the controller is the device used last. Options > Controls:
  the keyboard page's labels come from the menu string table (scripts.arc strings.dat, by key
  through 0x4938ed): with the controller used last its key column names the controller's
  buttons; the joystick page's row names (built from DirectInput object names in 0x48ad88, by
  binding code) are the controller's for pad.c's layout. Mouse-only menu buttons (that page's
  tab and arrows): the right stick moves the game's pointer and A clicks where it is until the
  D-pad or left stick is used again.
- Steering: the game steers by the joystick's X in proportion (0x457f34: |x| / 32767 when the
  binding is analogue, type 3; no dead zone of its own, DIPROP_RANGE +-32767). pad.c's round
  dead zone (option controller_deadzone, 6 %) comes first; option steering = soft / softer bends
  the left stick's x (power 1.6 / 2.3, full lock unchanged) for a short thumbstick; linear is
  as shipped.
- Movies (runtime/game.c, hook on 0x4130e3, a movie's frame): any key or controller button
  ends the movie playing at its next frame. The game itself ends one on WM_KEYDOWN, but the
  legal screen at start-up plays in a loop of WinMain's that reads no messages; presses there
  waited in the queue and then skipped the intro instead. The press that ends a movie has its
  queued key messages dropped. Option intro_videos = off gives the legal screen and the intro
  the one-frame black movie that videos = off gives every movie.

### 7. Menus added (runtime/menus.c, 2026-10-25)
- The game's menus are text scripts (UIData scripts.arc, 320x256 layout) read by name through
  the archive reader 0x467b4a; menus.c changes two as they are read and makes one:
  Options (o_op00.afw) gets a seventh button, More Options (o_nat.afw, new): the runtime's
  options as rows (display, resolution, widescreen, HUD, anti-aliasing, texture filter, smooth
  motion, v-sync, frame limit, rumble, movies, cheats), changed with Left / Right / A, saved to
  options.txt and applied at once (texture filtering at the next start). Their actions
  ("Native_Next" / "Native_Prev") are taken before the shell's dispatcher 0x480bd8; the values
  are written into the page after it loads (0x480f1c) with the shell's own text setter 0x48dc1c.
  Labels come through the string table hook (game.c).
- Cheats: the object at 0x4d4170 (+0x104 unlocked, +0x108 on, bit n cheat n). Codes (XOR 0x61
  at 0x4bc6e8: 0 ILIKETOCHEAT debug keys, 1 BEANY all trucks, 2 LEAD TO ROME all tracks, 3 MAD
  HOOKUP all upgrades, 4 SAD CLOWN all divisions, 5 KIYON unlimited cash, 6 TURN TRICKS stunt,
  7 CHING KUNG big air, 8 PLUMBER lowrider, 9 YOKOZUNA sumo, 10 MARS MODE low gravity, ZAKARY X
  all) typed as a name unlock and turn on a cheat; the Cheat Menu (o_op05.afw) toggles the
  unlocked ones among 5-10 and its Apply carries out 1-4 (0x47307e). Option cheats = all (the
  default, as the author asked) keeps 1-10 unlocked; the menu gets rows for 1-4 (the string table
  already names them), with its Next / Prev (0x472f6b), SetCurCheat (0x472c45) and on / off
  display (0x472fe6) taught about them.

### 8. Testing
- A --test mode with a virtual clock and fixed inputs; screenshots off-screen; few game runs
  (read the code first).
- Reference build (ref/ref_cpu.c, forked from 1NSANE's; make build/tdor3_ref, Unicorn from a
  Python unicorn package): the original machine code of every module run in Unicorn on the same
  runtime. tools/lockstep.py runs it and the translated build with TDOR3_PLAIN=1 (the runtime's
  hooks into the game's functions off) and the same scripted keys / controller, compares memory
  every frame and narrows a difference to one runtime call (tools/memdiff.py).

## Game facts learned

- Boot order (TDOR3.exe WinMain 0x450c2e): Framewerk log (c:\fw_tdor3.log), CD check
  (GetDriveTypeA on every drive, then D:\Tracks), tdor3.cfg, window class "Test Drive Off-Road 3"
  (popup 320x240, wndproc 0x44d35a), "Loading..." drawn with DrawTextA, network layer "Demon"
  (WSAStartup, a socket), DirectSound buffers for every sound in Sounds.zip (WAVs parsed with
  mmio on memory), CD audio (MCI) and the mixer's CD line volume, DirectInput (keyboard, mouse,
  joysticks), sr.dll's timer calibration (srTimer: Sleep(5000) against RDTSC the first time, the
  result stored in its registry key; 0.5 s check on later starts), the SurRender plug-ins
  (srEXT_Default, srEXT_unzip, srEXT_BINK by LoadLibrary), UIData/fonts.arc, Static.zip,
  road.fx, then the display driver: srDD_Glide3x, srDD_Glide2x, srDD_DirectX6 in turn.
- Main loop (0x450c2e): PeekMessage; when idle, elapsed = timeGetTime() delta (ms, reset above
  1000), then 0x44fcd5(elapsed): the race adds it to an accumulator (0x4e6850) and runs fixed
  steps of [0x4b4730] ms while it exceeds one step (at most 5 steps' worth kept), then draws.
  Physics is frame-rate independent; above the step rate frames repeat the last state.
- CD audio (0x453040-0x4538d7): MCI cdaudio by type id 0x204, time format TMSF, status READY,
  NUMBER_OF_TRACKS, LENGTH/POSITION of a track (MSF minutes*60+seconds), MODE; play, stop,
  close. Volume (0x453443 read, 0x4535b5 write): the first mixer's line of component type
  SRC_COMPACTDISC, its VOLUME control, 0..65535 (tdor3.cfg MusicVolume).
- Movies: srEXT_BINK's video stream (vtable 0x100031a0) reads the Bink handle's width +0,
  height +4, frame count +0x10, current frame +0x14, frame rate +0x1c / +0x20; frames by
  BinkGoto/BinkDoFrame, then BinkCopyToBuffer into a SurRender colour surface (formats: 24-bit
  and 32-bit, either channel order; flag 0x4000000 copy all). Config keys BINK_SOUNDSYSTEM,
  BINK_OPEN_FLAGS. Files "Movies\%s.bik" on the CD.

## Progress

- 2026-10-24: modules analysed (work/analysis/: modules.py, crt.py, survey.py, trial.py,
  srdd_iface.txt), plan above.
- 2026-10-25: translator extended to several modules (tools/image.py, recomp.py; PE rebase and
  exports in pe.py; import slots of translated exports decoded as direct calls in x86.py;
  discovery: relocated code pointers trusted as entries, switch bounds through stack variables,
  function starts after jump tables, rejected candidates looked at again each round).
  13,639 functions, 1.49M lines, one minute to translate and to compile (JOBS=6).
  Runtime forked from 1NSANE's and trimmed; new: loader.c (modules, DllMain, LoadLibrary,
  GetProcAddress, host modules), kernel32x.c, crt.c (memcpy & co. on the host), wsock32.c,
  music.c (MCI cdaudio + mixer over MUSIC/TrackNN.ogg with FFmpeg), srdd.c (driver stub that
  logs), vk.c + shaders (Vulkan 1.3 renderer core). Boot reaches the native driver's first
  calls (getDriverInfo, createContext, getInfo, getTextureFormats).
- 2026-10-25: discovery cross-checked against Ghidra 12.1.4 (the rip's full analysis of the same
  files; work/analysis/xcheck_*.txt): every function Ghidra finds that discovery missed is
  unreferenced (TDOR3.exe 140, sr.dll 36; REA xrefs on samples: no code or data references),
  plus 186 Ghidra starts inside discovered sr.dll bodies. All 365 added as EXTRA_ENTRIES
  (dead code translated as insurance; sr.dll bodies split where Ghidra splits them): 14,027
  functions. Boot unchanged (reaches srDD::getTextureFormats).
- 2026-10-25: MAIN MENU. The driver interface was worked out from DX6, Software and sr.dll into
  work/analysis/SRDD_SPEC.md (all 43 methods, every struct; positions are homogeneous and go
  through the one projection matrix the engine sends, GL clipping; srShader is one u32). The
  native driver (runtime/srdd.c) and renderer (vk.c, shaders/) follow it: ARGB8888 textures
  uploaded at bindTexture, CPU buffer spans for bufferOp, the shader word decoded in draw.frag.
  Bink stand-in fixed (surface type 2 is 32-bit). Headless --test run: legal screen, intro movie
  (FFmpeg), CD music track 6 from MUSIC/Track06.ogg, main menu at frame ~4100 (work/shots4/c.png).
  Windowed run: Vulkan swapchain on the 890M with vsync, movies with sound.
  Next: the window's size (640x480 on a 2560x1600 screen; scale up, full screen), input
  (menus by keyboard and pad), a race, then the enhancements (resolution, widescreen,
  frame pacing above the physics rate).
- 2026-10-25: RACES. Fixed on the way:
  - Translator: a call whose fall-through runs into another function now hands that function
    the flags the callee returns (codegen.py liveness). MSVCRT's cos/sin (0x78028c00) test ZF
    set by their argument loader 0x7802c2fa after the call; cos(0) came out NaN, the car's
    wheel frames NaN from the first physics step, the camera NaN, every visibility test
    (srGERD::testBoundingSphere) false: an empty world with only sky and HUD.
  - srTimer calibration: the registry's HKCU\SOFTWARE\Hybrid\hrtimer is set at start to the
    runtime's RDTSC rate (400 MHz, cpuid.c) with the CPUID it checks (advapi32.c): no 5 s wait
    on the first start, and right in --test (where Sleep did not pass virtual time; it does
    now: vclock_advance).
  - KERNEL32 GetFileTime, GetFileInformationByHandle, SetFileTime, LockFile, UnlockFile
    (srEXT_Unzip opening track zips).
  Debug aids: TDOR3_FPE_FROM=<frame> traps invalid x87/SSE operations on the game thread
  (fpu_host_mask); TDOR3_SMOOTH_LOG=1 logs the blend per frame. gdb with breakpoints on F_<addr>
  and watchpoints on program memory (identity-mapped) found the NaN chain quickly.
  Headless --test run: Arcade > name > track (Yosemite) > Hummer > race; driven with scripted
  keys and with a scripted controller (TDOR3_PAD: A through menus, RT throttle, stick steer).
  Screenshots work/shots5/ (psheet.png, gsheet.png).
  Next: a real run on the desktop (full screen, 180 Hz monitor, a real controller), gamma
  (srDD::update), race end / pause / quit paths, sound check in races, rumble (the game's
  force feedback?), the controls page with Xbox names, network play stays off.
- 2026-10-25: USER32 accelerators (LoadAcceleratorsA by name from the exe's resources,
  TranslateAcceleratorA -> WM_COMMAND): pause, cameras, ignition, lights and the rest had been
  dead. Videos off hands over a one-frame black movie (a failed BinkOpen is an uncaught C++
  exception). Rumble synthesized from the player car's velocity change per step (the game has
  no force feedback of its own; game.c). Pause / quit to menus / a second race: fine.
  Debug aids: TDOR3_SKIP_TEX=a,b leaves draws with those textures out; TDOR3_PROBE=x,y with
  TDOR3_LOG_FRAME logs which draws change (or cover) a target pixel.
- 2026-10-25: supersampling and anisotropic filtering; app-menu launcher
  ~/.local/share/applications/tdor3-native.desktop ("Test Drive Off-Road 3 (Linux port)", user
  folder ~/.local/share/tdor3-native, log last-run.log there; icon of the author's own). Hot-plug
  checked with a uinput virtual Xbox pad: picked up mid-movie, its A
  skipped the intro. Menus run ~2900 fps with 4x at 2048x1280 and vsync off.
  C++ exceptions: still unsupported (fatal with the type named). Throw sites: 11 sr.dll
  functions through its _CxxThrowException thunk 0x10077a9e (srIOManager errors and the like),
  srEXT_BINK's thunk, MSVCRT/MSVCP60 internals; none fire in normal play so far (a failed
  BinkOpen did; avoided).
  Next: a race played by the author on the desktop with a real controller (feel of steering dead zone,
  triggers, rumble strength); World Tour; the controls page prompts; exceptions if any appear.
- 2026-10-25 (after the author's first play: "fine"; menus had cut-off elements; controls page):
  - Cut-off text in menus: the glyph cells in the menu fonts end half a texel short (a
    letter's top row lies just outside its quad's texture coordinates). Direct3D 6 sampled a
    pixel at its top-left corner (the DX6 driver maps clip space to x*w/2 + w/2 - 1/1024), so the
    row still showed there; Vulkan samples at the centre. 2D draws now sample textures half a
    game pixel up-left (draw.frag, push d3d_shift; widened scenes 0).
  - The mouse: SDL window coordinates go through the letterbox and scaling to the game's
    640x480 (vk_window_to_game), GetCursorPos follows; GetSystemMetrics reports the game's
    640x480 screen (the menu framework, 0x475d91, scales positions by its desktop over it).
  - Controls page as above; the controller's pointer for mouse-only buttons.
  - Debug: TDOR3_DUMP_TEX=dir writes each texture's first level (.pam); TDOR3_LOG_VERTS=n.
- 2026-10-25: exceptions (decision 4 above) and WORLD TOUR, headless with scripted keys and
  TDOR3_AUTOPILOT (the game's AI drives the player's car: control mode car+0x5c0, 1 player /
  2 AI, set by 0x446cef(car, mode, skill); player car 0x4e4380 = [0x4b483c]):
  start new tour, name, showroom (buy / sell dialogs, credits), select division (cup info,
  purse), track data, garage, three races of the first cup (Vermont, the Mayan jungle,
  Canadian Rockies) with results, race credits / points / place in cup, the high-score table,
  then the next cup's first race. Load/Save Game: slots C:\TDOR3\savedata\SlotDataN.zah
  (written to <user>/SaveData), saved, loaded in a fresh run, Continue Tour with the credits
  kept. No exceptions thrown in all of it. Dialogs (purchase, sell) draw on black: the game's
  own (no back-buffer reads).
- 2026-10-25 (after the author's second play: skip intro option, skip movies with the controller,
  all cheats in a menu from the start, an options menu with more options, text cut off on car
  select, black edges on the speedometer, driving felt a little weird):
  - Movies: any key or button ends one, the legal screen included (decision 6); option
    intro_videos.
  - Menus (decision 7): Options > More Options (14 rows of the runtime's options) and the Cheat
    Menu with all ten cheats unlocked from the start (option cheats = all).
  - Car select: its stat labels and the pointer were queued before the turning truck's scene
    and drawn inside it (widened, unshifted); the scene hook flushes SurRender's batches on both
    sides now (decision 5).
  - Speedometer: dark lines along its pieces' edges from the filter reaching round the image;
    2D pieces showing a whole image are sampled within it (decision 5).
  - Driving: the reference build (decision 8) ran the menus and ~25 s of racing (analogue
    steering, throttle, brake) frame for frame identical to the translated build, so the
    driving is the game's own code exactly; smoothing on / off leaves the physics bit-identical
    (TDOR3_CAR_LOG=1 logs the player's car each step); input and the chase camera are scaled by
    elapsed time (only the draw distance looks at the frame rate). One difference from the
    1999 machine remained: the stick (linear on a short thumbstick; option
    steering = soft / softer, controller_deadzone, both in More Options).
- 2026-11-01: f59116f hung every normal start at the legal screen, black, for good (found when
  the Steam Deck stayed black; the laptop did the same). It set the x87 to single precision at
  srDD::createContext on the guess that Direct3D 6 had left it so. [verified] srTimer's time is
  near 2.3e10 s (2^63 ticks at the runtime's 400 MHz; a movie's +0x20 read 23058432421.7 with
  the game's control word, 23058432000 with single precision), where 24 bits resolve 2048 s:
  time stands still, a movie's frame stays -1 (62 frames by 5.9 s with the game's word). So the
  game cannot have run in single precision here and the change is gone, with its option fpu.
  It got through because every test after it ran with intro_videos = off; the two lockstep runs
  with it prove nothing about it (both builds froze alike). A test of anything global must
  include a run with the movies on.
  Compiled pipelines are saved between runs (<user>/cache/pipelines.bin, as td6-native; the
  log's last lines give the counts). [verified] First-run stutter on the 890M, the driver's own
  disk cache off, start-up through menus and a race: 4 pipelines, 6.8 ms in all, slowest 6.7 ms
  (one uber-shader, few blend states); from the saved cache 0.2 ms, frame identical. Not
  noticeable, so no list of pipelines to rebuild at start-up.
