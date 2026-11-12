# Files Riley writes for the dock

Riley's Settings (Dock page) and its theme switcher write the files that the dock reads. This page is the contract
between the two repositories: what each file holds, how the dock reads it, and how a writer must replace it. Every rule
cites the Rust dock it comes from (riley `dock/src/<file>.rs:<line>`); the C++ core in `src/core/` ports the same rules
function for function and is checked against the Rust tests by `tests/run-core.sh`.

Paths are relative to `$XDG_CONFIG_HOME` (falling back to `$HOME/.config`), except the pins file, which lives under
`$XDG_CACHE_HOME` (falling back to `$HOME/.cache`) for compatibility with nwg-dock (riley dock/src/main.rs:10-14,
dock/src/ui.rs:75-77).

| File | Path | Written by | Read by the dock |
|---|---|---|---|
| Dock config | `nwg-dock-hyprland/riley.conf` (under `$XDG_CONFIG_HOME`) | Settings > Dock | at start, then again whenever it changes |
| Pins | `nwg-dock-pinned` (under `$XDG_CACHE_HOME`) | Settings > Dock, and the dock itself (right-click Pin / Unpin) | at start, then again whenever it changes |
| Style (Rust dock) | `riley-dock/style.css` (under `$XDG_CONFIG_HOME`) | the theme switcher | at start, then again whenever it changes |
| Style (Qt dock) | `riley-dock/style.json` (under `$XDG_CONFIG_HOME`) | the theme switcher, from milestone 3 | at start, then again whenever it changes |

## 1. Dock config: `nwg-dock-hyprland/riley.conf`

### Format

- Plain UTF-8 text, one `KEY=value` per line. Lines end in `\n`; a `\r\n` ending is accepted (Rust `str::lines`
  drops the `\r`, and every value below is trimmed).
- A file that is missing or cannot be read as UTF-8 is treated as empty, so every key takes its default
  (riley dock/src/main.rs:66, dock/src/ui.rs:78 and :291, `read_to_string(..).unwrap_or_default()`).
- Unknown keys and lines without `=` are ignored. Writers must keep them: Settings rewrites only the line of the key
  it changes and leaves every other line as it was.
- Except for `ICON_SIZE` (see below), a line is split at its first `=`, the key is trimmed of Unicode white space and
  compared exactly (case-sensitive), and **the last matching line wins**.
- Trimming is Rust's `str::trim` (Unicode White_Space). The C++ core uses the same set.

### Keys

| Key | Type | Default | Meaning | Rust | C++ core |
|---|---|---|---|---|---|
| `ICON_SIZE` | unsigned 32-bit integer, > 0 | `44` | Icon size in pixels | dock/src/rules.rs:101-104 | `rules::iconSize` |
| `SCREENS` | comma-separated list of text | empty | Which screens get a dock, matched against the monitor description | dock/src/rules.rs:118-130, :159-171 | `rules::screenFilters`, `rules::selectMonitors` |
| `REFLECTIONS` | `0` or anything else | on | Icon reflections on the shelf | dock/src/rules.rs:132-140 | `rules::reflectionsEnabled` |
| `LAUNCHER` | `left`, `right`, `center` or `off` | `left` | Where the app-grid button goes in the row | dock/src/grid.rs:7-16 | `grid::position` |
| `AUTOHIDE` | `1` or `0` | off | Auto-hide: the dock slides away and leaves a 6 px lip | dock/src/autohide.rs:23-31 | `autohide::enabled` |

#### `ICON_SIZE`

- Matched by prefix, not by trimmed key: only a line that starts with exactly `ICON_SIZE=` counts (no space before
  the key or around `=`). Its value is trimmed and parsed as an unsigned 32-bit integer (Rust `u32::from_str`: digits
  with an optional leading `+`; no sign `-`, no fraction, nothing above 4294967295).
- **The first line with a valid value above 0 wins** (`find`), not the last; invalid or zero values are skipped.
  No valid line gives 44 (riley dock/src/rules.rs:102-103).
- The icon is drawn a quarter larger and, past six apps, shrinks in integer steps: `size * 5 / 4 * 6 / (6 + (apps - 6)
  / 3)` in integer arithmetic, where `apps` counts apps, not the launcher (riley dock/src/rules.rs:106-111).
- Settings writes it as a plain decimal integer (riley settings/riley_settings/dock_core.py:88-89).

#### `SCREENS`

- The value of the last `SCREENS` line is split at `,`; each part is trimmed, empty parts are dropped, and a part
  already in the list is dropped (first one kept, order kept) (riley dock/src/rules.rs:118-130).
- A screen gets a dock when its Hyprland monitor `description` **contains** any part (substring, case-sensitive)
  (riley dock/src/rules.rs:165-167).
- When the list is empty or nothing matches, the dock goes on one screen: the lowest `y`, then the lowest `x`, then the
  smallest name (riley dock/src/rules.rs:168-170), and the dock logs the fallback (riley dock/src/main.rs:78-81).
- At start, with `SCREENS` empty, the fallback is used at once. With `SCREENS` set but not matched, the dock keeps
  asking Hyprland for the monitor list (every 250 ms, for up to 10 seconds) in case a matching screen appears, and uses
  the last fallback it saw when the time is up (riley dock/src/main.rs:67-90, dock/src/rules.rs:149-157).
- `--output NAME` on the command line overrides `SCREENS`: exactly that monitor, or none (riley
  dock/src/rules.rs:160-164).
- Read once at start only (riley dock/src/main.rs:65-66); a change needs a dock restart. Settings restarts the dock
  after changing it (riley settings/riley_settings/dock_core.py:176-178).
- Writers: a part may not contain `,`, `\r` or `\n`; Settings writes the trimmed, de-duplicated parts joined by `,`
  (riley settings/riley_settings/dock_core.py:106-114).

#### `REFLECTIONS`

- Starts on. Every `REFLECTIONS` line sets it to "trimmed value is not `0`", so the last one wins and any value other
  than `0` (including an empty or invalid one) means on (riley dock/src/rules.rs:132-139).
- `--no-reflections` on the command line turns reflections off whatever the file says (riley dock/src/main.rs:38,
  dock/src/rules.rs:139 `allowed && enabled`).
- Settings writes `1` or `0` (riley settings/riley_settings/dock_core.py:146-147).

#### `LAUNCHER`

- The trimmed value of the last `LAUNCHER` line: `right`, `center` or `off` (exact, lower case); anything else,
  including a missing key, is `left` (riley dock/src/grid.rs:8-15).
- `left` puts the grid button first, `right` last, `center` in the middle of the row (at index `(n + 1) / 2` among the
  `n` other items, Rust `div_ceil(2)`), and `off` leaves it out (riley dock/src/grid.rs:18-26).

#### `AUTOHIDE`

- On only when the trimmed value of the last `AUTOHIDE` line is exactly `1`; missing, `0` or anything else is off
  (riley dock/src/autohide.rs:22-31).
- Applied live without a restart, so the dock never drops and re-takes its reserved space (riley
  dock/src/ui.rs:296-301).

### When the dock reads it

The dock watches the file (moves included) and on every change re-reads `ICON_SIZE`, `REFLECTIONS`, `LAUNCHER` and
`AUTOHIDE` and redraws (riley dock/src/ui.rs:218-229, :290-295). `SCREENS` is not re-read (see above).

### How a writer replaces it

Write the whole new text to a temporary file in the same folder, flush and `fsync` it, then rename it over
`riley.conf`, so the dock never reads a half-written file. Settings does exactly this (riley
settings/riley_settings/dock_core.py:61-85: temporary `.riley-conf-*` in the same folder, `fsync`, `os.replace`). It
replaces the first line of the key in place, keeping that line's `\r\n` or `\n` ending, drops later lines of the same
key, and appends `KEY=value\n` when the key is missing.

## 2. Pins: `nwg-dock-pinned` (under `$XDG_CACHE_HOME`)

### Format

- UTF-8 text, one app per line, in dock order (riley dock/src/pins.rs:6-12).
- Each line is trimmed; empty lines are skipped. A missing file is an empty list. Any other read error (including
  invalid UTF-8) is an error, and the dock keeps the list it already has (riley dock/src/pins.rs:9-10,
  dock/src/ui.rs:292 and :841).
- An entry is a desktop-file id without `.desktop` (for example `org.mozilla.firefox`) or a window class. The dock finds
  the app by `<entry>.desktop`, then `<entry lower-cased>.desktop`, then a desktop file whose `StartupWMClass` equals the
  entry ignoring case (riley dock/src/desktop.rs:18-40).
- Writers write each entry followed by `\n` (riley dock/src/pins.rs:28).

### Order

- The row is: the launcher, then the pins in file order, then running apps that are not pinned (riley
  dock/src/rules.rs:31-48). A repeated pin is shown once, at its first place (riley dock/src/rules.rs:33-36).
- A window joins the pinned item whose entry equals its class exactly (case-sensitive); other windows are sorted by
  workspace id, then class, and form new items (riley dock/src/rules.rs:38-46).
- Where the launcher finally sits is set by `LAUNCHER` (section 1).
- Pin adds the entry at the end unless it is already there (exact match); Unpin removes every equal entry and keeps the
  others in order (riley dock/src/pins.rs:36-42). Settings can also move an entry to any place (riley
  settings/riley_settings/dock_core.py:186-200).

### How it is replaced

- The dock saves by writing a unique temporary file `.dock-pins-<pid>-<n>.tmp` in the same folder (created exclusively,
  retrying the next `n` if it exists), writing every entry, `fsync`, then renaming it over the pins file; on any error
  the temporary file is removed and the old pins file stays (riley dock/src/pins.rs:14-34). The folder is created
  first if missing (riley dock/src/pins.rs:16-17).
- Before a Pin or Unpin the dock re-reads the file, so a change written by Settings is not lost (riley
  dock/src/ui.rs:841-843).
- The dock watches the file and reloads it on every change (riley dock/src/ui.rs:218-229, :290-292).
- Other writers must also replace the file by a rename from the same folder, never by writing it in place. Settings
  writes `nwg-dock-pinned.new` and renames it over the file (riley settings/riley_settings/dock_core.py:19-23).

## 3. Style

### 3.1 Rust dock: `riley-dock/style.css`

- The current Rust dock reads **GTK CSS** from `riley-dock/style.css` (riley dock/src/ui.rs:84-86). The theme switcher
  writes it (riley bin/hypr-theme.py:321-334).
- If the file cannot be read, the dock uses the built-in example style (riley dock/src/ui.rs:282-285,
  `dock/style.example.css`).
- The dock watches the file and reloads it on every change (riley dock/src/ui.rs:207-217).
- Until the switch to the Qt dock (milestone 3), the theme switcher keeps writing this file for the Rust dock.

### 3.2 Qt dock: `riley-dock/style.json`

GTK CSS does not apply to Qt Quick. The Qt dock reads its colours, fonts and the few sizes the Rust dock takes from
its CSS from a small JSON file, `riley-dock/style.json` under `$XDG_CONFIG_HOME` (falling back to `$HOME/.config`),
next to the CSS file. From milestone 3 the theme switcher writes it from the same theme values it uses for the CSS
today. `style.example.json` at the top of this repository holds the defaults; they equal riley
`dock/style.example.css`.

#### Format

- One UTF-8 JSON object. Keys are the names below; their order does not matter. Unknown keys are ignored (the example
  file's first key, `SPDX-License-Identifier`, is one of them).
- **Colour**: a string `#rrggbb` (opaque) or `#rrggbbaa`, hexadecimal digits in either case. The alpha byte comes
  **last**, as in CSS; this is not Qt's `#aarrggbb` string form, and the dock parses it itself. A CSS
  `alpha(#rrggbb, a)` or `rgba(r, g, b, a)` becomes `aa = floor(a * 255 + 0.5)` (0.72 -> `b8`, 0.35 -> `59`,
  0.18 -> `2e`, 0.94 -> `f0`, 0.3 -> `4d`); note that Python's `round()` rounds 76.5 down, so writers use the formula.
  Names (`red`), `#rgb` and `rgba(...)` are bad values.
- **Size**: a JSON integer, in logical pixels (CSS `px`), at least 0. A font size is a pixel size (Qt
  `font.pixelSize`, CSS `font-size: 12px`) and must be at least 1.
- **Font family**: a non-empty string, one family name without quotes (`Inter`). When the family is not installed, Qt's
  font matching picks a sans-serif face, as the CSS fallback `Inter, sans-serif` does.

#### Keys

Every property the Rust dock's CSS actually uses, with its CSS source (riley `dock/style.example.css`, line) and where
the Rust dock uses it.

| Key | Type | Default | Meaning | CSS (style.example.css) | Rust use |
|---|---|---|---|---|---|
| `bar_background` | colour | `#1a1b26b8` | Fill of the bar's glass | `.dock-bar` `background: alpha(#1a1b26, 0.72)` (:4) | view.rs:63, :90 |
| `bar_foreground` | colour | `#c0caf5` | Colour of symbolic icons and of the app-grid button's dots at rest | `.dock-bar` `color` (:5) | view.rs:121, :135, :159-176 |
| `bar_border` | colour | `#7aa2f759` | The bar's outline | `.dock-bar` `border: 1px solid alpha(#7aa2f7, 0.35)` (:6) | view.rs:90 |
| `bar_border_width` | size | `1` | Width of that outline | same (:6) | view.rs:90 |
| `bar_radius` | size | `18` | Corner radius of the bar | `.dock-bar` `border-radius` (:7) | view.rs:90 |
| `accent` | colour | `#7aa2f7` | Running-app dots, the auto-hide lip dots and the app-grid button's hover colour | `.dock-indicator` `color` (:16) | view.rs:89, :93, :141, :164-166 |
| `shelf` | colour | `#7aa2f72e` | The 1 px shelf line the reflections stand on | `.dock-shelf` `background: alpha(#7aa2f7, 0.18)` (:15) | view.rs:64-67, :126 |
| `active_line` | colour | `#7aa2f7` | The line under the focused app | `.dock-item.active` `border-bottom: 3px solid #7aa2f7` (:17) | view.rs:68-77, :154 |
| `active_line_width` | size | `3` | Thickness of that line | same (:17) | view.rs:68-77 |
| `name_background` | colour | `#1a1b26f0` | Fill of the hover name label | `.dock-name` `background: alpha(#1a1b26, 0.94)` (:25) | ui.rs:105-117 |
| `name_foreground` | colour | `#c0caf5` | Text colour of the name label | `.dock-name` `color` (:25) | ui.rs:115-116 |
| `name_border` | colour | `#7aa2f74d` | Outline of the name label | `.dock-name` `border: 1px solid alpha(#7aa2f7, 0.3)` (:26) | ui.rs:115-116 |
| `name_border_width` | size | `1` | Width of that outline | same (:26) | ui.rs:115-116 |
| `name_radius` | size | `8` | Corner radius of the name label | `.dock-name` `border-radius` (:26) | ui.rs:115-116 |
| `name_padding_x` | size | `11` | Left and right padding inside the name label | `.dock-name` `padding: 4px 11px` (:27) | ui.rs:115-116 |
| `name_padding_y` | size | `4` | Top and bottom padding inside the name label | same (:27) | ui.rs:115-116 |
| `name_font_family` | font family | `Inter` | Font of the name label | `.dock-name` `font-family: Inter, sans-serif` (:27) | ui.rs:115-116 |
| `name_font_size` | size (pixels) | `12` | Font size of the name label | `.dock-name` `font-size: 12px` (:27) | ui.rs:115-116 |
| `menu_padding` | size | `6` | Padding inside the right-click menu | `.dock-menu` `padding` (:29) | ui.rs:766 |

The theme switcher's values for the same keys today: `bar_background` and `name_background` are the theme's `bg`
(alpha 0.72 and 0.94), `bar_foreground` and `name_foreground` its `fg_bright`, `accent`, `active_line` and `shelf`
its `accent` (alpha 1, 1 and 0.18), `bar_border` and `name_border` its `accent` (alpha 0.35 and 0.3), and
`name_font_family` the theme's dock font, `Inter` by default (riley bin/hypr-theme.py:62, :321-334). Its font-only
update (riley bin/hypr-theme.py:107-108) changes `name_font_family` alone.

#### CSS that is not carried over

These rules are in the CSS but change nothing the Rust dock draws, so the JSON file has no key for them:

- `window.dock-window, window.dock-name-window { background: transparent }` (:2): the Qt dock's surfaces are always
  transparent.
- `.dock-bar { padding: 6px 10px }` (:8): the bar is an empty box placed at the computed bar rectangle (riley
  dock/src/view.rs:63, :199-202), so padding paints nothing; the bar's geometry comes from the dock's layout rules.
- `.dock-item` and `.dock-item:hover, .dock-item:active` (:10-11): they reset the box of the focused-app line, which
  can never be hovered or pressed (riley dock/src/view.rs:213-219). The line's size and place are fixed by the dock
  (riley dock/src/view.rs:72-76).
- `.dock-icon`, `.dock-item:hover .dock-icon`, `.dock-item:active .dock-icon`, `@keyframes dock-bounce` and
  `.dock-item.launching .dock-icon` (:12-14, :18-23): no widget of the Rust dock has the `dock-icon` or `launching`
  class (the classes it sets are in riley dock/src/ui.rs:94, :106, :116, :766 and dock/src/view.rs:200-215); the
  swell, press and launch bounce are computed by the dock. The theme switcher does not write these rules either
  (riley bin/hypr-theme.py:322-334).
- `.dock-indicator { font-size: 10px; min-height: 8px }` (:16): the indicator label is never shown; only its colour
  is read (riley dock/src/view.rs:89, :203-212).
- `.dock-shelf { min-height: 1px }` (:15): the shelf is placed at its computed rectangle (riley dock/src/view.rs:67).

#### Missing file, missing key, bad value

- **Missing file**: every key takes its default, with no message, as the Rust dock falls back to its built-in example
  style (riley dock/src/ui.rs:282-285).
- **Unreadable file, invalid JSON, or JSON that is not an object**: every key takes its default, and the dock logs one
  line to its log (`riley/dock.log` under `$XDG_STATE_HOME`, as riley dock/src/main.rs:16-22).
- **Missing key**: that key takes its default; the others keep their values.
- **Bad value** (wrong JSON type, a colour that is not `#rrggbb` or `#rrggbbaa`, a negative or fractional size, a font
  size of 0, an empty font family): that key alone takes its default, and the dock logs one line naming the key. GTK
  does the same for a bad CSS declaration, which it skips while keeping the others.

#### When the dock reads it

At start, then again whenever it changes: the dock watches the file, including a replace by rename, re-reads the whole
file, applies the rules above again (so a key that was removed goes back to its default) and redraws. The Rust dock
does the same with its CSS (riley dock/src/ui.rs:207-217: a file monitor with moves, `load_css`, then a redraw of every
dock). The style never changes the surface size, the input region or the reserved space, so a reload never moves
tiled windows.

#### How a writer replaces it

As the dock config (section 1): write the whole new file to a temporary file in the same folder, flush and `fsync`
it, then rename it over `style.json`, so the dock never reads a half-written file.
