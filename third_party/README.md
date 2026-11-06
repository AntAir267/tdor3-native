Third-party sources fetched and built locally (not committed):

- SDL3 (zlib licence), 3.4 or later. If your system has SDL3's development package
  (`pkg-config --modversion sdl3` answers), the Makefile uses it and nothing is needed here.
  Otherwise build the official source release from github.com/libsdl-org/SDL, unpacked here as
  SDL3-<version>/, into sdl3/ as a static library with its headers:

      cmake -S SDL3-<version> -B SDL3-build -DSDL_SHARED=OFF -DSDL_STATIC=ON \
            -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/sdl3"
      cmake --build SDL3-build -j8 && cmake --install SDL3-build

  (or point the Makefile at another copy: `make SDL3=/path/to/sdl3`).

- Unicorn (GPL 2), only for the reference build that checks the translation
  (`make build/tdor3_ref`, `tools/lockstep.py`): `.venv/bin/pip install unicorn` puts the
  library where the Makefile looks.
