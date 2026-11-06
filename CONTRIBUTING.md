# Contributing

Issues and pull requests are welcome. A few rules keep the project possible:

- **No game files, ever.** Nothing from the game (its code, data, art, sound, movies, text),
  nothing extracted or converted from it, and no generated C from the translator. Addresses,
  hashes and notes on how the game works are fine. Bring your own game to test.
- **The checks must pass.** A change to the translator or to anything the game's code runs on
  keeps the reference build identical (`make build/tdor3_ref`, then `tools/lockstep.py` with a
  scripted run; see DESIGN.md, "Testing"). Test with the movies on as well as off.
- **Settings live in the game's own menus.** Anything a player can change goes in
  Options > More Options (runtime/menus.c), drawn in the game's style; a file or an environment
  variable alone is not enough. The game as shipped stays reachable, and no graphics option
  changes how the game plays.
- **Every controller works.** Any connected controller drives player 1, plugged in before or
  after the game starts, alongside the keyboard.
- Describe what you changed and how you checked it. Pull requests are read in full before
  anything in them is built or run, and merged by the maintainer.

The goal: the game as its makers would have built it with unlimited time, without changing how it
plays.
