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
| Dock config | `riley-dock/config.xml` (under `$XDG_CONFIG_HOME`), schema `schemas/config.xsd` | Settings > Dock, once it writes XML | at start, then again whenever it changes |
| Dock config, old | `nwg-dock-hyprland/riley.conf` (under `$XDG_CONFIG_HOME`) | Settings > Dock, today | fallback only, until Riley's Settings writes config.xml (section 1.1) |
| Pins | `nwg-dock-pinned` (under `$XDG_CACHE_HOME`) | Settings > Dock, and the dock itself (right-click Pin / Unpin) | at start, then again whenever it changes |
| Style (Rust dock) | `riley-dock/style.css` (under `$XDG_CONFIG_HOME`) | the theme switcher | at start, then again whenever it changes |
| Style (Qt dock) | `riley-dock/style.xml` (under `$XDG_CONFIG_HOME`), schema `schemas/style.xsd` | the theme switcher, from milestone 3 | at start, then again whenever it changes |

Both XML files follow the rules of section 4 (parsing, validation and the one error line).

## 1. Dock config: `riley-dock/config.xml`, fallback `nwg-dock-hyprland/riley.conf`

All of Riley's configuration is XML with an XML schema, so the dock's config is `riley-dock/config.xml` under
`$XDG_CONFIG_HOME`, checked against `schemas/config.xsd`. Until Riley's Settings writes that file, it keeps writing the
old `nwg-dock-hyprland/riley.conf`, and the dock reads that one as a fallback. Both files hold the same five settings
with the same meanings, ranges and defaults; `config.example.xml` at the top of this repository is a config.xml with
every default.

### 1.1 Which file the dock reads

At start, in this order (riley-dock `src/app/config.cpp`, `loadConfig`):

1. `config.xml`, when it is present and valid (it parses and validates against `schemas/config.xsd`, section 4).
2. Otherwise `riley.conf`, read by the rules of section 1.4, exactly as before config.xml existed. A missing riley.conf
   is an empty one.
3. Otherwise, with neither file, the built-in defaults (they are what an empty riley.conf gives).

A config.xml that is present but fails to parse or validate is not used at all, not even in part: the dock writes the
one error line (section 4, "Errors") and reads riley.conf instead.

Later, whenever either file changes (the dock watches both, moves included), the same order is applied again, with one
difference: **a config.xml that fails to parse or validate keeps the settings in use**, whatever file they came from,
and writes its error line. A config.xml that is removed hands over to riley.conf again. While a config.xml exists,
riley.conf is read only at start, when config.xml is broken.

What a reload changes is as before (riley dock/src/ui.rs:218-229, :290-295): the icon size, reflections, the launcher
place and auto-hide are applied and the docks redrawn; auto-hide is switched live, without a restart, so the dock never
drops and re-takes its reserved space (riley dock/src/ui.rs:296-301). The screens are read once, at start only (riley
dock/src/main.rs:65-66); a change needs a dock restart, and Settings restarts the dock after changing them (riley
settings/riley_settings/dock_core.py:176-178).

### 1.2 The settings

| Setting | config.xml element | XSD type | riley.conf key | Default | Rust | C++ core |
|---|---|---|---|---|---|---|
| Icon size | `<icon-size>` | `xs:unsignedInt`, 1 or more, digits with an optional `+` | `ICON_SIZE` | `44` | dock/src/rules.rs:101-104 | `rules::iconSize` |
| Screens | `<screens>` holding `<screen>` elements | each `xs:string`: one line, no `,`, not only white space | `SCREENS` | none | dock/src/rules.rs:118-130, :159-171 | `rules::screenFilters`, `rules::selectMonitors` |
| Reflections | `<reflections>` | `xs:boolean` | `REFLECTIONS` | on (`true`) | dock/src/rules.rs:132-140 | `rules::reflectionsEnabled` |
| Launcher place | `<launcher>` | `left`, `right`, `center` or `off` | `LAUNCHER` | `left` | dock/src/grid.rs:7-16 | `grid::position` |
| Auto-hide | `<autohide>` | `xs:boolean` | `AUTOHIDE` | off (`false`) | dock/src/autohide.rs:23-31 | `autohide::enabled` |

The dock maps either file onto the same five values (riley-dock `src/app/config.h`, `struct DockConfig`), and from
there on nothing depends on which file they came from.

- **Icon size**: in logical pixels. The icon is drawn a quarter larger and, past six apps, shrinks in integer steps:
  `size * 5 / 4 * 6 / (6 + (apps - 6) / 3)` in integer arithmetic, where `apps` counts apps, not the launcher (riley
  dock/src/rules.rs:106-111). Largest value 4294967295, as Rust's `u32`.
- **Screens**: a screen gets a dock when its Hyprland monitor `description` **contains** one of the texts (substring,
  case-sensitive) (riley dock/src/rules.rs:165-167). Each text is trimmed of Unicode white space (Rust `str::trim`), and
  an empty text or a repeat of an earlier one is dropped (first one kept, order kept) (riley dock/src/rules.rs:118-130).
  When the list is empty or nothing matches, the dock goes on one screen: the lowest `y`, then the lowest `x`, then the
  smallest name (riley dock/src/rules.rs:168-170), and the dock logs the fallback (riley dock/src/main.rs:78-81). At
  start, with no screens listed, the fallback is used at once. With screens listed but none matched, the dock keeps
  asking Hyprland for the monitor list (every 250 ms, for up to 10 seconds) in case a matching screen appears, and uses
  the last fallback it saw when the time is up (riley dock/src/main.rs:67-90, dock/src/rules.rs:149-157). `--output NAME`
  on the command line overrides the list: exactly that monitor, or none (riley dock/src/rules.rs:160-164).
- **Reflections**: icon reflections on the shelf. `--no-reflections` on the command line turns them off whatever the
  file says (riley dock/src/main.rs:38, dock/src/rules.rs:139 `allowed && enabled`).
- **Launcher place**: `left` puts the app-grid button first, `right` last, `center` in the middle of the row (at index
  `(n + 1) / 2` among the `n` other items, Rust `div_ceil(2)`), and `off` leaves it out (riley dock/src/grid.rs:18-26).
- **Auto-hide**: the dock slides away and leaves a 6 px lip.

### 1.3 `config.xml`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<dock xmlns="urn:riley-dock:config" version="1">
  <icon-size>44</icon-size>
  <screens>
    <screen>DELL U3824DW</screen>
  </screens>
  <reflections>true</reflections>
  <launcher>left</launcher>
  <autohide>false</autohide>
</dock>
```

- The root is `dock` in the namespace `urn:riley-dock:config`, with `version="1"` (required; this schema knows only
  version 1). The general rules of section 4 apply: UTF-8, no DOCTYPE, at most 1 MiB, validated as a whole.
- Every element is optional, each may appear at most once, and they may come in any order. A missing element takes
  its default, so `<dock xmlns="urn:riley-dock:config" version="1"/>` is a config with every default. Any other element
  or attribute makes the file invalid.
- `<icon-size>`, `<reflections>`, `<launcher>` and `<autohide>` may have white space around their value (XML Schema
  collapses it). `<reflections>` and `<autohide>` take `true`, `false`, `1` or `0`. `<launcher>` is lower case.
- `<screens>` holds zero or more `<screen>` elements, in order; an empty `<screens/>` is the same as none. A `<screen>`
  is one part of the old comma-separated `SCREENS` list: it may not hold `,` or a line break and must hold something
  other than spaces; the dock trims it (above). Text is XML text: write `&amp;` for `&` and `&lt;` for `<`.
- Writers write the whole file and replace it by a rename, as for riley.conf (section 1.4, "How a writer replaces
  it"). Settings should write every element, in the order above, so the file reads as a complete list.

### 1.4 `riley.conf` (fallback only, until Riley's Settings writes config.xml)

The old file, `nwg-dock-hyprland/riley.conf` under `$XDG_CONFIG_HOME`. The dock reads it only as section 1.1 says.

#### Format

- Plain UTF-8 text, one `KEY=value` per line. Lines end in `\n`; a `\r\n` ending is accepted (Rust `str::lines`
  drops the `\r`, and every value below is trimmed).
- A file that is missing or cannot be read as UTF-8 is treated as empty, so every key takes its default
  (riley dock/src/main.rs:66, dock/src/ui.rs:78 and :291, `read_to_string(..).unwrap_or_default()`).
- Unknown keys and lines without `=` are ignored. Writers must keep them: Settings rewrites only the line of the key
  it changes and leaves every other line as it was.
- Except for `ICON_SIZE` (see below), a line is split at its first `=`, the key is trimmed of Unicode white space and
  compared exactly (case-sensitive), and **the last matching line wins**.
- Trimming is Rust's `str::trim` (Unicode White_Space). The C++ core uses the same set.
- Unlike config.xml, a bad value never makes the whole file unusable: each key below says what a bad value gives.

#### `ICON_SIZE`

- Matched by prefix, not by trimmed key: only a line that starts with exactly `ICON_SIZE=` counts (no space before
  the key or around `=`). Its value is trimmed and parsed as an unsigned 32-bit integer (Rust `u32::from_str`: digits
  with an optional leading `+`; no sign `-`, no fraction, nothing above 4294967295).
- **The first line with a valid value above 0 wins** (`find`), not the last; invalid or zero values are skipped.
  No valid line gives 44 (riley dock/src/rules.rs:102-103).
- Settings writes it as a plain decimal integer (riley settings/riley_settings/dock_core.py:88-89).

#### `SCREENS`

- The value of the last `SCREENS` line is split at `,`; each part is then a screen text of section 1.2 (riley
  dock/src/rules.rs:118-130).
- Writers: a part may not contain `,`, `\r` or `\n`; Settings writes the trimmed, de-duplicated parts joined by `,`
  (riley settings/riley_settings/dock_core.py:106-114).

#### `REFLECTIONS`

- Starts on. Every `REFLECTIONS` line sets it to "trimmed value is not `0`", so the last one wins and any value other
  than `0` (including an empty or invalid one) means on (riley dock/src/rules.rs:132-139).
- Settings writes `1` or `0` (riley settings/riley_settings/dock_core.py:146-147).

#### `LAUNCHER`

- The trimmed value of the last `LAUNCHER` line: `right`, `center` or `off` (exact, lower case); anything else,
  including a missing key, is `left` (riley dock/src/grid.rs:8-15).

#### `AUTOHIDE`

- On only when the trimmed value of the last `AUTOHIDE` line is exactly `1`; missing, `0` or anything else is off
  (riley dock/src/autohide.rs:22-31).

#### How a writer replaces it

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
- Where the launcher finally sits is set by the launcher place (`<launcher>`, or `LAUNCHER` in riley.conf; section 1.2).
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

### 3.2 Qt dock: `riley-dock/style.xml`

GTK CSS does not apply to Qt Quick. The Qt dock reads its colours, its font and the few sizes the Rust dock takes from
its CSS from a small XML file, `riley-dock/style.xml` under `$XDG_CONFIG_HOME` (falling back to `$HOME/.config`), next
to the CSS file, checked against `schemas/style.xsd`. From milestone 3 the theme switcher writes it from the same theme
values it uses for the CSS today. `style.example.xml` at the top of this repository holds the defaults; they equal
riley `dock/style.example.css`.

#### Format

```xml
<?xml version="1.0" encoding="UTF-8"?>
<style xmlns="urn:riley-dock:style" version="1">
  <bar-background>#1a1b26b8</bar-background>
  <accent>#7aa2f7</accent>
  <name-font-family>Inter</name-font-family>
  <name-font-size>12</name-font-size>
</style>
```

- The root is `style` in the namespace `urn:riley-dock:style`, with `version="1"` (required; this schema knows only
  version 1). The general rules of section 4 apply: UTF-8, no DOCTYPE, at most 1 MiB, validated as a whole.
- Every element is optional, each may appear at most once, and they may come in any order. A missing element takes
  its default. Any other element or attribute makes the file invalid. White space around a value is allowed.
- **Colour** (`colour` in the schema): `#rrggbb` (opaque) or `#rrggbbaa`, hexadecimal digits in either case. The alpha
  byte comes **last**, as in CSS; this is not Qt's `#aarrggbb` string form, and the dock parses it itself. A CSS
  `alpha(#rrggbb, a)` or `rgba(r, g, b, a)` becomes `aa = floor(a * 255 + 0.5)` (0.72 -> `b8`, 0.35 -> `59`,
  0.18 -> `2e`, 0.94 -> `f0`, 0.3 -> `4d`); note that Python's `round()` rounds 76.5 down, so writers use the formula.
  Names (`red`), `#rgb` and `rgba(...)` are invalid.
- **Size** (`size`): a whole number of logical pixels (CSS `px`), written without a unit: digits with an optional `+`,
  0 to 2147483647.
- **Font size** (`fontSize`): a pixel size (Qt `font.pixelSize`, CSS `font-size: 12px`), written as a size, 1 or more.
- **Font family** (`fontFamily`): one family name without quotes (`Inter`), not empty; runs of white space count as one
  space (`xs:token`). When the family is not installed, Qt's font matching picks a sans-serif face, as the CSS fallback
  `Inter, sans-serif` does.

#### Elements

Every property the Rust dock's CSS actually uses, with its CSS source (riley `dock/style.example.css`, line) and where
the Rust dock uses it.

| Element | Type | Default | Meaning | CSS (style.example.css) | Rust use |
|---|---|---|---|---|---|
| `bar-background` | colour | `#1a1b26b8` | Fill of the bar's glass | `.dock-bar` `background: alpha(#1a1b26, 0.72)` (:4) | view.rs:63, :90 |
| `bar-foreground` | colour | `#c0caf5` | Colour of symbolic icons and of the app-grid button's dots at rest | `.dock-bar` `color` (:5) | view.rs:121, :135, :159-176 |
| `bar-border` | colour | `#7aa2f759` | The bar's outline | `.dock-bar` `border: 1px solid alpha(#7aa2f7, 0.35)` (:6) | view.rs:90 |
| `bar-border-width` | size | `1` | Width of that outline | same (:6) | view.rs:90 |
| `bar-radius` | size | `18` | Corner radius of the bar | `.dock-bar` `border-radius` (:7) | view.rs:90 |
| `accent` | colour | `#7aa2f7` | Running-app dots, the auto-hide lip dots and the app-grid button's hover colour | `.dock-indicator` `color` (:16) | view.rs:89, :93, :141, :164-166 |
| `shelf` | colour | `#7aa2f72e` | The 1 px shelf line the reflections stand on | `.dock-shelf` `background: alpha(#7aa2f7, 0.18)` (:15) | view.rs:64-67, :126 |
| `active-line` | colour | `#7aa2f7` | The line under the focused app | `.dock-item.active` `border-bottom: 3px solid #7aa2f7` (:17) | view.rs:68-77, :154 |
| `active-line-width` | size | `3` | Thickness of that line | same (:17) | view.rs:68-77 |
| `name-background` | colour | `#1a1b26f0` | Fill of the hover name label | `.dock-name` `background: alpha(#1a1b26, 0.94)` (:25) | ui.rs:105-117 |
| `name-foreground` | colour | `#c0caf5` | Text colour of the name label | `.dock-name` `color` (:25) | ui.rs:115-116 |
| `name-border` | colour | `#7aa2f74d` | Outline of the name label | `.dock-name` `border: 1px solid alpha(#7aa2f7, 0.3)` (:26) | ui.rs:115-116 |
| `name-border-width` | size | `1` | Width of that outline | same (:26) | ui.rs:115-116 |
| `name-radius` | size | `8` | Corner radius of the name label | `.dock-name` `border-radius` (:26) | ui.rs:115-116 |
| `name-padding-x` | size | `11` | Left and right padding inside the name label | `.dock-name` `padding: 4px 11px` (:27) | ui.rs:115-116 |
| `name-padding-y` | size | `4` | Top and bottom padding inside the name label | same (:27) | ui.rs:115-116 |
| `name-font-family` | font family | `Inter` | Font of the name label | `.dock-name` `font-family: Inter, sans-serif` (:27) | ui.rs:115-116 |
| `name-font-size` | font size | `12` | Font size of the name label | `.dock-name` `font-size: 12px` (:27) | ui.rs:115-116 |
| `menu-padding` | size | `6` | Padding inside the right-click menu | `.dock-menu` `padding` (:29) | ui.rs:766 |

The theme switcher's values for the same elements today: `bar-background` and `name-background` are the theme's `bg`
(alpha 0.72 and 0.94), `bar-foreground` and `name-foreground` its `fg_bright`, `accent`, `active-line` and `shelf`
its `accent` (alpha 1, 1 and 0.18), `bar-border` and `name-border` its `accent` (alpha 0.35 and 0.3), and
`name-font-family` the theme's dock font, `Inter` by default (riley bin/hypr-theme.py:62, :321-334). Its font-only
update (riley bin/hypr-theme.py:107-108) changes `name-font-family` alone.

#### CSS that is not carried over

These rules are in the CSS but are not style elements:

- `window.dock-window, window.dock-name-window { background: transparent }` (:2): the Qt dock's surfaces are always
  transparent.
- `.dock-bar { padding: 6px 10px }` (:8): the bar is an empty box placed at the computed bar rectangle (riley
  dock/src/view.rs:63, :199-202), so padding paints nothing; the bar's geometry comes from the dock's layout rules.
- `.dock-item { padding: 0; margin: 0 1px; ... }` (:10): the focused-app line is drawn in a 20 x 3 px box centred
  under its icon (riley dock/src/view.rs:72-76), and GTK draws that box inside its `margin: 0 1px`, so **the line is
  18 px wide**, 1 px in from each side of the box. The live run measured the Rust dock's line as 18 px (22.5 device
  pixels at scale 1.25 on DP-2). The Qt dock draws it the same way, at a fixed 18 px (riley-dock
  src/app/dockwindow.cpp:418-427); the margin is not a style element, since the theme switcher never changes it (riley
  bin/hypr-theme.py:322-334). The rest of the rule (`padding: 0`, transparent background, no border, no shadow) and
  `.dock-item:hover, .dock-item:active` (:11) change nothing drawn: the line can never be hovered or pressed (riley
  dock/src/view.rs:213-219).
- `.dock-icon`, `.dock-item:hover .dock-icon`, `.dock-item:active .dock-icon`, `@keyframes dock-bounce` and
  `.dock-item.launching .dock-icon` (:12-14, :18-23): no widget of the Rust dock has the `dock-icon` or `launching`
  class (the classes it sets are in riley dock/src/ui.rs:94, :106, :116, :766 and dock/src/view.rs:200-215); the
  swell, press and launch bounce are computed by the dock. The theme switcher does not write these rules either
  (riley bin/hypr-theme.py:322-334).
- `.dock-indicator { font-size: 10px; min-height: 8px }` (:16): the indicator label is never shown; only its colour
  is read (riley dock/src/view.rs:89, :203-212).
- `.dock-shelf { min-height: 1px }` (:15): the shelf is placed at its computed rectangle (riley dock/src/view.rs:67).

#### Reading it

- **Missing file**: every element takes its default, with no message, as the Rust dock falls back to its built-in
  example style (riley dock/src/ui.rs:282-285).
- **A file that is present and valid**: its values, and the default for every element it leaves out.
- **A file that cannot be read, parse or validate**: the one error line (section 4, "Errors"), and the style in use is
  kept: the defaults at start, else the style of the last good read. Unlike GTK with a bad CSS declaration, no part of
  an invalid file is used.

At start, then again whenever it changes: the dock watches the file, including a replace by rename, re-reads the whole
file, applies the rules above again (so an element that was removed goes back to its default) and redraws. The Rust
dock does the same with its CSS (riley dock/src/ui.rs:207-217: a file monitor with moves, `load_css`, then a redraw of
every dock). The style never changes the surface size, the input region or the reserved space, so a reload never moves
tiled windows.

#### How a writer replaces it

As the dock config (section 1.4): write the whole new file to a temporary file in the same folder, flush and `fsync`
it, then rename it over `style.xml`, so the dock never reads a half-written file.

## 4. Reading the XML files

`config.xml` and `style.xml` are read the same way (riley-dock `src/app/xmlfile.cpp`, `readValidated`):

- The whole file is read into memory, at most 1 MiB (a larger file is an error), and parsed with libxml2 from there.
  The parser never loads anything from outside the file: no network (`XML_PARSE_NONET`), no external entities or DTD
  (`XML_PARSE_NO_XXE`, and neither `XML_PARSE_NOENT` nor `XML_PARSE_DTDLOAD`), a resource loader that refuses every
  load, and no XInclude. A file with a DOCTYPE is an error.
- UTF-8 is expected; an XML declaration naming another encoding is honoured by libxml2.
- The file is then validated against its schema, `schemas/config.xsd` or `schemas/style.xsd`. The schemas are compiled
  into the dock as Qt resources (`:/riley-dock/schemas/`) and never read from disk, so the installed dock always checks
  against the schema it was built with. Riley's Settings and theme switcher can check a file before writing it with
  `xmllint --noout --schema schemas/config.xsd config.xml`, which gives the same verdict.
- Only a file that validates is used. Values are then read as the schema reads them: white space collapsed for
  booleans, numbers, the launcher place, colours and the font family; `<screen>` text trimmed by the screen rule of
  section 1.2.
- `version="1"` on the root is required. A later format gets a new version number and schema; a dock that does not
  know it treats the file as invalid (the error line names the version) and keeps to its fallback.

### Errors

A file that is present but cannot be read, parsed or validated writes exactly one line to the dock's stderr, and the
same line to its log (`riley/dock.log` under `$XDG_STATE_HOME`, riley dock/src/main.rs:16-22):

```
riley-dock-qt: config error: <file>:<line>: <message>
riley-dock-qt: style error: <file>:<line>: <message>
```

- `riley-dock-qt` is the program's name (it becomes `riley-dock` at the switch). `config` or `style` names the file
  kind. `<file>` is the full path the dock read. `<line>` is the 1-based line of the first error libxml2 reported, or
  `0` when there is none (the file could not be read, or is larger than 1 MiB). `<message>` is libxml2's message for
  that first error, without its line end (for example `Element '{urn:riley-dock:config}launcher': [facet
  'enumeration'] The value 'middle' is not an element of the set {'left', 'right', 'center', 'off'}.`), or one of the
  dock's own: `a DOCTYPE is not allowed`, `the file is larger than 1 MiB`, or the system's read error.
- Only the first error is reported; libxml2 warnings are not errors. Control characters in the line become spaces, so
  it is always one line.
- The line never contains `WARNING`, `CRITICAL` or `panicked`, the words the capture gates look for in the dock's
  stderr: inside the line they are written `warning`, `critical` and `PANICKED`.
- The same line for the same file is written once. Reading the same broken file again (another change event, the
  second read at start) writes nothing more; the line is written again only after that file was read without an error
  or was missing in between.

### What the schemas cannot say

- The screen rule's trimming uses Unicode white space (Rust `str::trim`), the schema only XML white space: a
  `<screen>` of nothing but, for example, no-break spaces validates and is then dropped, as the same `SCREENS` part is.
- Repeated `<screen>` texts validate; the dock keeps the first (section 1.2). The schema could forbid exact repeats
  only, not repeats that differ in surrounding white space.
- Whether a font family is installed, and that it is one name rather than a CSS list, cannot be checked by a schema.
- The 1 MiB limit and the DOCTYPE ban are the dock's rules, not the schema's; `xmllint --schema` accepts a valid file
  with a DOCTYPE that the dock refuses.
