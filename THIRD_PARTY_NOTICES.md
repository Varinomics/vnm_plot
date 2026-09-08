# Third-Party Notices

The source code in this repository is licensed under the BSD 2-Clause License
in `LICENSE.txt`. No font file is checked in here, but the build puts third-party
font bytes into the artifacts it produces, and those fonts keep their own
licences. Anyone distributing a binary built from this repository distributes
those bytes and carries the notices below with them.

Both fonts come from [vnm_fonts](https://github.com/Varinomics/vnm_fonts), which
ships them byte-verbatim as their authors published them and records the
upstream repository, revision, path, digest and size of each one.

| Font | Where it ends up | Licence |
|---|---|---|
| Ubuntu Mono - Bront | `vnm_plot_rhi`, as a C++ byte array under the asset name `fonts/monospace.ttf` | Ubuntu Font Licence 1.0 |
| Font Awesome 7 Free Solid | `function_plotter`, in the example's Qt resources | SIL Open Font License 1.1 |

## Ubuntu Mono - Bront

Embedded into `vnm_plot_rhi` from `fonts/UbuntuMono-Bront.ttf`, and baked into
an MSDF atlas at runtime. The typeface is Chris Wendt's derivative of Canonical's
Ubuntu Mono, distributed by him as `Ubuntu Mono - Bront`; the file is his,
unmodified.

License: Ubuntu Font Licence 1.0.
Local license text: `LICENSES/Ubuntu-Font-Licence-1.0.txt`.

Copyright notice carried in the font:

- Copyright 2011 Canonical Ltd. Licensed under the Ubuntu Font Licence 1.0.

Source:

- https://github.com/chrismwendt/bront
- https://ubuntu.com/legal/font-licence

`fonts/monospace.ttf` is the name `Font_renderer` asks its `Asset_loader` for.
It is this project's asset namespace, not a claim about which typeface is
behind it: a consumer that registers other font bytes under that name gets its
own typeface and its own atlas, and then carries its own notices.

## Font Awesome 7 Free Solid

Compiled into the `function_plotter` example's Qt resources from
`fonts/FontAwesome7Free-Solid.otf`, where the example's toolbar loads it and
draws its play, pause, close and add glyphs.

License: SIL Open Font License 1.1.
Local license text: `LICENSES/FontAwesome-7.2.0-OFL-1.1.txt`, the 7.2.0
release's own licence file.

Copyright notice carried in the font:

- Copyright (c) Font Awesome. The release's licence file states the notice as
  `Copyright (c) 2026 Fonticons, Inc. (https://fontawesome.com)`.

Source:

- https://github.com/FortAwesome/Font-Awesome
- https://fontawesome.com/license/free

## The fonts are not registered through vnm_fonts' library

vnm_fonts serves a file contract and a library contract, and only the files are
used here. Its `vnm::fonts` library marks a family name on the way into
`QFontDatabase`, because two files declaring one family name merge there into a
single entry and glyph lookup and rasterisation can then be served from
different files. Nothing in `vnm_plot` itself enters a font database: the
monospace face is baked into an MSDF atlas straight from its bytes, so there is
no family entry to collide with and nothing for the mark to do. `vnm::fonts` is
therefore deliberately not linked, and adding the marking would change nothing.
