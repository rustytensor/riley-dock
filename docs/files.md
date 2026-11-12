# Files Riley writes for the dock

Riley's Settings (Dock page) and its theme switcher write three files that the dock reads. This page is the contract
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
| Style | `riley-dock/style.css` (under `$XDG_CONFIG_HOME`) | the theme switcher | at start, then again whenever it changes |

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

## 3. Style: `riley-dock/style.css`

- The current Rust dock reads **GTK CSS** from `riley-dock/style.css` (riley dock/src/ui.rs:84-86). The theme switcher
  writes it (riley bin/hypr-theme.py:322).
- If the file cannot be read, the dock uses the built-in example style (riley dock/src/ui.rs:282-285,
  `dock/style.example.css`).
- The dock watches the file and reloads it on every change (riley dock/src/ui.rs:207-217).
- GTK CSS does not apply to Qt Quick. The Qt dock's style format (colours and fonts, as a small JSON or INI file, with
  its path and its fallback) is defined in milestone 2; the theme switcher will then write that file instead. Until the
  switch to the Qt dock, the theme switcher keeps writing `riley-dock/style.css` for the Rust dock.
