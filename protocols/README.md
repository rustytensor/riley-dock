<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Wayland protocols

Protocol files used by the build, copied unchanged from their upstream repositories. Each keeps its own copyright
and licence text; its licence is also named in a `.license` file beside it.

| File | Upstream | Licence | Used by |
|---|---|---|---|
| `wlr-virtual-pointer-unstable-v1.xml` | wlroots `wlr-protocols`, `unstable/wlr-virtual-pointer-unstable-v1.xml` (https://gitlab.freedesktop.org/wlroots/wlr-protocols), master at commit `1106a4a1c57562ddd438c547da4c0d6dc150d624`; SHA-256 `3ff6d540be0bc5228195bf072bde42117ea17945a5c2061add5d3cf97d6bb524` | MIT (Copyright © 2019 Josef Gajdusek; the text is in the file's `<copyright>` element) | `gates/pointer/` (`riley-dock-pointer`, the capture gate's pointer helper; never installed) |

The MIT licence lets these files be used in a GPL-3.0-or-later project; their own notice must stay with them, so they
are never edited. To update one, copy the new upstream file over it and change the commit and checksum above.
