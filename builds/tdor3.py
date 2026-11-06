"""Test Drive Off-Road 3 (1999, PC CD): the files the user's installation must hold, where each
translated module is placed, and what the translator needs to know about them (addresses
only; DESIGN.md decision 2)."""

NAME = 'Test Drive Off-Road 3 (1999)'

# Translated modules: file in the game folder, SHA-256, address it runs at (DLLs are rebased
# there; sr.dll and the C runtimes keep their own bases). `load`: 'start' modules are loaded
# before the program starts (the exe's own imports), 'dynamic' ones when the program asks
# (LoadLibrary: SurRender's plug-ins).
MODULES = [
    dict(file='TDOR3.exe', base=0x00400000, load='start',
         sha256='824859526fd7809a0b2aa80584aab3b2ebba1a3e6be2f538262e1269ca99abc5'),
    dict(file='MSVCRT.DLL', base=0x78000000, load='start',
         sha256='d4287ab5e4988dfe99bd54243d50dbe8744094f11fe5f9809a1a6fb9728c2124'),
    dict(file='MSVCP60.DLL', base=0x780c0000, load='start',
         sha256='2b6b93c2d66969eb00258e2b5ad6172decebada096e3b1b077a3380c80e4a072'),
    dict(file='sr.dll', base=0x10000000, load='start',
         sha256='4e5a36a78462a12f51b10b6d0e4819ca6439f97ffe9823bf5ead83ed30e00278'),
    dict(file='srVP_Generic.dll', base=0x10600000, load='dynamic',
         sha256='212354d6f98d2b9f2f77e1f53edabcce6f0a75743c3efa2e42ab84079b3db60c'),
    dict(file='srEXT_Unzip.dll', base=0x10610000, load='dynamic',
         sha256='16436f902781693bfd8e53217765c39e02cc53ff20723dcec96f371b4fb26ca1'),
    dict(file='srEXT_default.dll', base=0x10630000, load='dynamic',
         sha256='279ecb01395147ed18f2a80f59a28b18056e4e8c9fc12e544e9c58e42da83401'),
    dict(file='srEXT_BINK.dll', base=0x10640000, load='dynamic',
         sha256='dd90f68915d2b45e0d2427a25abc3692dd7904898bebb2c4fd853b9066cee969'),
]

# Host imports that never return
NORETURN_IMPORTS = ['ExitProcess', 'ExitThread', 'FatalAppExitA', 'FatalExit']

# Translated functions that never return (by export name, module file: name)
NORETURN_EXPORTS = [
    ('MSVCRT.DLL', '_CxxThrowException'), ('MSVCRT.DLL', 'longjmp'), ('MSVCRT.DLL', 'exit'),
    ('MSVCRT.DLL', '_exit'), ('MSVCRT.DLL', 'abort'), ('MSVCRT.DLL', '_amsg_exit'),
    ('MSVCRT.DLL', '?terminate@@YAXXZ'), ('MSVCRT.DLL', '_endthread'), ('MSVCRT.DLL', '_endthreadex'),
]

# Function starts Ghidra 12.1.4 finds that discovery does not reach (cross-check 2026-10-25: REA
# finds no references to a sample of them, so they are dead code; translated anyway, and
# sr.dll's that sit inside a discovered body split it where Ghidra does)
EXTRA_ENTRIES = [
    0x00401000, 0x004013f9, 0x00401844, 0x004018e3, 0x00402c34, 0x00402e93, 0x0040cc62, 0x0040e8c1,
    0x00412c22, 0x00419e99, 0x0041a51d, 0x0041d7b0, 0x0041e4ce, 0x0041eeec, 0x0041f197, 0x0041f5d7,
    0x0042254d, 0x00422a91, 0x0043441f, 0x004360ec, 0x0043d4d2, 0x0044816f, 0x004482ae, 0x004483ed,
    0x004484a8, 0x0044b268, 0x0045334d, 0x0045395e, 0x00453a1d, 0x004540d9, 0x00454a6e, 0x0045561d,
    0x00456b7e, 0x00456be6, 0x00456c30, 0x00457047, 0x00457245, 0x0045740e, 0x00458557, 0x004586ae,
    0x00458739, 0x0045c121, 0x0045c1c4, 0x0045c582, 0x0045cab0, 0x0045cae8, 0x0045d03a, 0x0045d065,
    0x0045d090, 0x0045d14b, 0x0045d364, 0x0045d821, 0x0045d887, 0x0045de07, 0x0045e085, 0x0045e178,
    0x0045e237, 0x0045e5a0, 0x0045e606, 0x0045e7e6, 0x0045e852, 0x0045e863, 0x0045e9c8, 0x0045ea2f,
    0x0045eb2c, 0x0045ec2e, 0x0045f74f, 0x00460145, 0x00460d79, 0x004610b7, 0x00461124, 0x004613fa,
    0x004618d0, 0x0046193d, 0x00462766, 0x00462824, 0x00462976, 0x00463f2d, 0x00463f64, 0x00464200,
    0x0046420a, 0x00466c56, 0x0046716c, 0x004671b4, 0x0046792d, 0x00467ca6, 0x00467e00, 0x004680b0,
    0x004682b5, 0x00468581, 0x0046863f, 0x00468918, 0x004693ba, 0x004694ac, 0x00469521, 0x00469526,
    0x00469870, 0x00469895, 0x00469b81, 0x00469d21, 0x00469df2, 0x00469f0e, 0x0046a290, 0x0046a3a5,
    0x0046a4b6, 0x0046a702, 0x0046bc80, 0x0046bd16, 0x0046bf23, 0x0046e354, 0x0046edbd, 0x004710a1,
    0x004710e2, 0x0047246c, 0x004724d3, 0x00472954, 0x00475507, 0x00475d17, 0x004774f8, 0x0047751d,
    0x0048353e, 0x004835e4, 0x00483974, 0x004861db, 0x004861f8, 0x00486208, 0x00486228, 0x0048d496,
    0x0048d49c, 0x0048d8a7, 0x0048db80, 0x0048ddf6, 0x0048de4f, 0x0048dea8, 0x0048e009, 0x0048e0ff,
    0x0048e12e, 0x00491004, 0x00491c7c, 0x00495550, 0x00496d66, 0x0049e45a, 0x0049e47f, 0x10002b40,
    0x10002b70, 0x100032c0, 0x10003390, 0x10003460, 0x10003530, 0x100035d0, 0x10003610, 0x10003650,
    0x10003690, 0x10004060, 0x10004150, 0x10004680, 0x10004720, 0x10004760, 0x10005230, 0x100052b0,
    0x10005330, 0x100053d0, 0x10005410, 0x10007860, 0x1000b060, 0x1000b160, 0x1000b260, 0x1000b340,
    0x1000b450, 0x1000b580, 0x1000b6b0, 0x1000b7d0, 0x1000b8b0, 0x1000b9e0, 0x1000e0b0, 0x1000e130,
    0x1000e150, 0x1000e190, 0x1000e1d0, 0x1000eb70, 0x1000ebf0, 0x1000ec70, 0x1000f4f0, 0x1000f530,
    0x1000f680, 0x100100f0, 0x10011280, 0x10011390, 0x10011750, 0x10011e00, 0x10012410, 0x10012450,
    0x10012a80, 0x10012ac0, 0x10012b00, 0x100139a0, 0x10013a40, 0x10013ed0, 0x10014270, 0x100142b0,
    0x10014840, 0x100148e0, 0x10014920, 0x1001d0b0, 0x1001d250, 0x1001d290, 0x1001ed90, 0x1001efc0,
    0x1001f040, 0x1001f0c0, 0x1001f140, 0x1001f170, 0x1001f3e0, 0x1001f460, 0x1001f4e0, 0x1001f6c0,
    0x1001f6f0, 0x1001f720, 0x1001f740, 0x1001f760, 0x10020fa0, 0x10021520, 0x100215a0, 0x100215b0,
    0x100215d0, 0x10021a30, 0x10021a70, 0x10023b70, 0x100285f0, 0x10028690, 0x100286c0, 0x100286e0,
    0x10028bb0, 0x1002ac60, 0x1002aca0, 0x1002e160, 0x1002e1b0, 0x10030210, 0x10030280, 0x10030470,
    0x10030510, 0x100305d0, 0x10030fa0, 0x100326e0, 0x10032a10, 0x100332c0, 0x10033300, 0x10035dd0,
    0x10035e70, 0x10035eb0, 0x10036640, 0x100369a0, 0x10037350, 0x10037390, 0x10038c30, 0x10039240,
    0x10039940, 0x10039dd0, 0x1003a6a0, 0x1003c190, 0x1003c1d0, 0x1003c3d0, 0x1003c470, 0x1003c5c0,
    0x1003c620, 0x1003c680, 0x1003c6e0, 0x1003de90, 0x1003ded0, 0x1003df80, 0x1003e050, 0x1003e090,
    0x1003e5b0, 0x1003e650, 0x1003e9f0, 0x1003ec40, 0x1003ec80, 0x1003ecc0, 0x1003f7b0, 0x1003f7f0,
    0x1003fba0, 0x1003fbe0, 0x10040820, 0x100408c0, 0x10040900, 0x10045710, 0x10045790, 0x10045830,
    0x10045870, 0x10045c50, 0x100466f0, 0x10046960, 0x100469a0, 0x1004c190, 0x1004c1a0, 0x1004c390,
    0x1004c480, 0x1004c670, 0x1004c890, 0x1004cac0, 0x1004cca0, 0x1004cec0, 0x1004dbd0, 0x1004dd80,
    0x1004ddc0, 0x1004e7d0, 0x1004e810, 0x1004fc00, 0x1004fca0, 0x1004fce0, 0x1004ffe0, 0x10050080,
    0x100500c0, 0x10051e20, 0x10051ec0, 0x10051f00, 0x10052880, 0x10052920, 0x10052960, 0x10053a20,
    0x10053ac0, 0x10053b00, 0x10054810, 0x10054990, 0x100554e0, 0x10055580, 0x100555c0, 0x10057570,
    0x10057610, 0x10057650, 0x10058330, 0x100583d0, 0x10058410, 0x10058450, 0x1005e690, 0x1005e730,
    0x1005e770, 0x1005ec70, 0x1005ef40, 0x1005f420, 0x1005f4c0, 0x1005f500, 0x10063540, 0x100635e0,
    0x10063620, 0x100663e0, 0x10066480, 0x100664c0, 0x10067600, 0x10067820, 0x100678c0, 0x10067900,
    0x10067a0d, 0x10068200, 0x100682a0, 0x100682e0, 0x100688a0, 0x10068d00, 0x10068d40, 0x1006b090,
    0x1006b0d0, 0x1006bd10, 0x1006bd50, 0x10070970, 0x100709b0,
]
NOT_ENTRIES = []
JUMP_TABLES = {}
NONLOCAL_JUMPS = []

# Functions the runtime wraps or replaces (address -> hook name; runtime sets hook_<name>)
HOOKS = {
    # MSVCRT's hand-written memory and string routines (runtime/crt.c): host versions, faster,
    # and their computed jumps into shared tails are hard to bound statically
    0x780015fd: 'crt_memcpy', 0x78005802: 'crt_memmove', 0x78001000: 'crt_memset',
    0x7800270e: 'crt_strlen', 0x78002787: 'crt_strcpy', 0x78002dde: 'crt_strcat',
    0x78010de3: 'crt_strncat', 0x7800511e: 'crt_strncpy',
    # srScene::render(srGERD&, srCamera*): widescreen (runtime/scene.c)
    0x1005ea20: 'scene_render',
    # motion between physics steps (runtime/smooth.c)
    0x0044fcd5: 'race_loop', 0x00430e72: 'race_step', 0x0041e526: 'race_scenes', 0x004221e0: 'race_draw',
    0x10059d90: 'node_dirty', 0x100590a0: 'node_dtor',
    # the menu string table's lookup: controller names on the controls pages (runtime/game.c)
    0x004938ed: 'string_lookup',
    # the joystick controls page's row names (runtime/game.c)
    0x0048ad88: 'joy_label',
    # MSVCRT's _JumpToContinuation: the end of a C++ catch (runtime/eh.c)
    0x7800ac4f: 'jump_to_continuation',
    # the race HUD's overlays, anchored to the screen's edges in widescreen (runtime/hud.c)
    0x00414cb6: 'overlays_draw',
    # a movie's frame: skipped by any key or button (runtime/game.c)
    0x004130e3: 'movie_frame',
    # the menus: scripts read from the archive, set up when loaded, their actions; the Cheat
    # Menu's rows (runtime/menus.c)
    0x00467b4a: 'arc_read', 0x00480f1c: 'script_load', 0x00480bd8: 'menu_action',
    0x00472f6b: 'cheat_step', 0x00472c45: 'cheat_action', 0x00472fe6: 'cheat_show',
}
# MSVCRT's _ftol, done in place by the generated code (as 1NSANE's static one)
INLINE = {0x78001f14: 'ftol'}

# Named addresses for the runtime (GAME_<NAME> in gen/image.h)
DATA = {}
