# spinfetch

A system information tool with a **spinning 3D logo**. It is a fork of
[fastfetch](https://github.com/fastfetch-cli/fastfetch): the logo you know from
fastfetch is turned into a thin slab that turns around in your terminal while the
system information stays next to it.

- **Real logos.** The front of the slab is the original logo art in its real
  colors. All of fastfetch's builtin logos work (500+), or point it at a text
  file in fastfetch's logo syntax.
- **Light.** One small C binary, no extra dependencies for the animation. Only
  the cells that change are redrawn.
- **Configurable.** Speed, tilt, frame rate and size are options, and any
  fastfetch config keeps working.
- **Safe.** It runs in the alternate screen, restores your terminal on exit and
  on signals, and never animates with `--pipe` or `--json`.
- **Easy start.** `spinfetch --configure` is a short setup wizard with a live
  preview, and it also opens by itself the first time when there is no config.

## Usage

```sh
spinfetch                          # spins the logo of your distro, any key stops it
spinfetch -l gentoo                # pick a logo
spinfetch -l ~/mylogo.txt          # your own logo file
spinfetch --logo-variant minimal   # the small logo, if the distro has one
spinfetch --logo-spin-speed 2 --logo-spin-tilt 20
spinfetch --configure              # setup wizard
```

Config lives in `~/.config/spinfetch/config.jsonc`. If that file does not exist,
`~/.config/fastfetch/config.jsonc` is used, so your fastfetch setup carries over.

```jsonc
{
  "logo": {
    "type": "spin",
    "source": "gentoo",
    "variant": "full",          // or "minimal"
    "spin": { "speed": 1.0, "tilt": 0, "fps": 20, "sshSafe": false }
  },
  "modules": ["title", "separator", "os", "kernel", "cpu", "memory"]
}
```

`sshSafe` caps the frame rate for remote sessions. The edges of the slab use the
block characters `░▒▓█`, so your terminal font needs those.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build/spinfetch
```

Run the tests with `cd build && ctest`.

## Credits

- Based on [fastfetch](https://github.com/fastfetch-cli/fastfetch) by the
  fastfetch contributors, under the MIT license. The system detection, modules and
  logo art all come from there. The original README is kept as
  [README-fastfetch.md](README-fastfetch.md).
- The idea of a fetch tool with an animated logo comes from **Fetch by aerofyl**.
  spinfetch is not a copy of it: I only took the idea and built it differently,
  turning fastfetch's own logos into a real 3D slab instead of drawing something
  separate.
