# SPDX-License-Identifier: GPL-3.0-or-later
"""Milestone 2 gate: the dock window on DP-2, measured from Hyprland and compared with fixed expected values.

This ports the Rust dock's four capture gates (riley dock/gates/surface.py, reflections_capture.py,
launcher_capture.py, autohide_capture.py) so that they run against any dock binary with the same command line
(--output NAME, --no-swell, --control-socket PATH, --show) and the same pins file. The Qt dock decides PASS or FAIL.
The reference is Hyprswell (the Rust dock's successor, its own repository since 2026-11-12): when given, it runs each
scenario first in the same run, with its own names (layer namespace hyprswell, config
nwg-dock-hyprland/hyprswell.conf, style hyprswell/style.css). The Qt dock's surface sizes must equal its sizes, and its
own checks are printed as REF lines.

Safety, for a live session:
- Hyprland is only queried, read only (layers, monitors, cursorpos, clients). Nothing is dispatched.
- The pointer is moved only by gates/pointer (riley-dock-pointer), with the wlr virtual pointer protocol: motion
  only, never a button, axis or key. While it moves the pointer it shows a red ring at the scripted position in its
  own overlay layer (riley-dock-test-marker), hidden for each screenshot. One notification comes before the first
  move of a run. After each move the cursor must be at the scripted position (within 1 px), or the hover part fails
  with "pointer moved by someone else" and stops. The helper always puts the cursor back where it was; the gate then
  checks that it is back and that the ring's layer is gone. --no-hover leaves the pointer alone.
- DP-2 only, one test dock at a time, and only when DP-2 has no dock layer. Each test dock gets its own HOME and XDG
  config, cache and state folders under a temporary folder (XDG_DATA_HOME stays the real one, read only, for icons and
  desktop entries) and a private control socket that must differ from the session's socket.
- Each test dock is stopped by its own process ID. Afterwards DP-2's reserved area must be what it was before, and the
  session's own docks must be unchanged; if not, the gate stops at once.

Output: one line per check, "PASS <scenario>: <what> = <value>" or "FAIL <scenario>: <what> = <actual>, expected
<expected> (<source>)", "REF ..." lines for the reference dock, then "GATE PASS n/n" or "GATE FAIL passed/n ...".
"""
import argparse
from collections import namedtuple
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = 'DP-2'
NAMESPACE = 'riley-dock'
REFERENCE_NAMESPACE = 'hyprswell'   # Hyprswell's layer (hyprswell src/ui.rs:97)
REFERENCE_CONFIG = 'nwg-dock-hyprland/hyprswell.conf'   # hyprswell src/ui.rs:77
MARKER_NAMESPACE = 'riley-dock-test-marker'   # gates/pointer: the red ring's layer; never a dock layer
PROGRAM = 'riley-dock-qt'
# The only Hyprland requests this gate makes; all four are read-only queries.
QUERIES = frozenset({'layers', 'monitors', 'cursorpos', 'clients'})

# ---------------------------------------------------------------------------------------------------------------
# Fixed expected values. Sources: the Rust dock's capture gates and code in the riley repository ("riley <path>:<line>"),
# the Rust dock's recorded DP-2 captures, this repository's docs/files.md, and Marc's decisions of 2026-11-12. None of
# them is derived from the output of the dock under test.
# ---------------------------------------------------------------------------------------------------------------
APPEAR_SECONDS = 3.0        # riley dock/gates/surface.py:39, reflections_capture.py:99-102, launcher_capture.py:53-56
TOP_LEVEL = '2'             # riley dock/gates/surface.py:51 (layer-shell TOP)
OVERLAY_LEVEL = '3'         # layer-shell OVERLAY, the ring's layer (gates/pointer/main.cpp)
SETTLE_SECONDS = 10.0       # riley dock/gates/reflections_capture.py:49
QUIET_SECONDS = 1.0         # riley dock/gates/reflections_capture.py:58,63
MAX_SETTLED_HEIGHT = 161    # riley dock/gates/reflections_capture.py:57-59 (ICON_SIZE=44 gives at most 161)
CLEANUP_SECONDS = 2.0       # riley dock/gates/reflections_capture.py:143-146
SHOW_TIMEOUT = 8            # riley dock/gates/autohide_capture.py:76-77
SAMPLE_SECONDS = 0.02       # riley dock/gates/autohide_capture.py:69-71
SHOWN_HOLD_SECONDS = 1.0    # riley dock/gates/autohide_capture.py:90
SWELL_SECONDS = 0.65        # riley dock/gates/reflections_capture.py:113
DIAGNOSTICS = ('CRITICAL', 'WARNING', 'panicked', 'Theme parser error')   # riley dock/gates/reflections_capture.py:125
DIAGNOSTICS_SURFACE = ('CRITICAL', 'panicked', 'Theme parser error')     # riley dock/gates/surface.py:61
HIDDEN_ZONE = 6             # riley dock/gates/autohide_capture.py:61-62 (hidden reservation is exactly 6 px)
REST_ZONE = 86              # recorded Rust DP-2 capture, 2026-11-12 (reflections rest and launcher captures)
# Recorded Rust DP-2 surface sizes of 2026-11-12. They depend on how many apps were open then, so they are printed as
# REF information only; the size checks compare with Hyprswell measured in the same run (Marc, 2026-11-12).
REST_SIZE = (921, 144)          # reflections rest and launcher left/right/center
LAUNCHER_OFF_SIZE = (845, 144)  # launcher off
AUTOHIDE_SIZE = (884, 132)      # auto-hide, surface fixed in both poses

POINTER_TOLERANCE = 1.0     # px: Marc's decision 2026-11-12 (cursor on DP-2), as the lead set it out
NOTICE = 'Dock test: moving the cursor on DP-2 for a few seconds'   # Marc's decision 2026-11-12, word for word
NOTICE_LEAD_SECONDS = 2.0   # time to read the notification before the first move
HELPER_TIMEOUT = 6.0        # s for each helper answer (the helper itself gives its layer 5 s to map)

SRC_RECORDED = 'recorded Rust DP-2 capture, 2026-11-12'
SRC_ANCHOR = 'riley dock/src/ui.rs:98-102'   # TOP layer, bottom anchor only (centred), bottom margin 0
SRC_SAME_RUN = "Marc's decision 2026-11-12; Hyprswell measured in this run"
SRC_CURSOR = "Marc's decision 2026-11-12 (the test may move the cursor on DP-2)"
SRC_XML_ERRORS = 'docs/files.md section 4, Errors'

# A config.xml that breaks schemas/config.xsd on line 4 only (the launcher value), with a valid icon size that would
# show in the surface size if any part of the file were used (docs/files.md section 1.1: not even in part).
INVALID_CONFIG_XML = ('<?xml version="1.0" encoding="UTF-8"?>\n'
                      '<dock xmlns="urn:riley-dock:config" version="1">\n'
                      '  <icon-size>64</icon-size>\n'
                      '  <launcher>middle</launcher>\n'
                      '</dock>\n')
# docs/files.md section 4, Errors: "riley-dock-qt: config error: <file>:<line>: <message>", the full path the dock
# read, the line of libxml2's first error and its message; this message is the example written there.
INVALID_CONFIG_MESSAGE = ("4: Element '{urn:riley-dock:config}launcher': [facet 'enumeration'] The value 'middle' "
                          "is not an element of the set {'left', 'right', 'center', 'off'}.")


class SafetyError(Exception):
    """A live-session rule would be broken: the gate stops at once."""


class Missing(Exception):
    pass


class HoverStopped(Exception):
    """The hover part of the run stops: no more pointer moves in this run."""


# --- Hyprland, read only ----------------------------------------------------------------------------------------

def query(what):
    if what not in QUERIES:
        raise SafetyError(f'hyprctl {what} is not a read-only query this gate may make')
    result = subprocess.run(['hyprctl', what, '-j'], capture_output=True, text=True, check=True, timeout=2)
    return json.loads(result.stdout)


def namespace_layers(namespace):
    # Exact namespace only: the dock's name popup (riley-dock-name) and the ring (riley-dock-test-marker) are not
    # dock layers, and no dock layer is a ring.
    return [(screen, level, row) for screen, entry in query('layers').items()
            for level, rows in entry['levels'].items() for row in rows
            if row.get('namespace') == namespace]


def dock_layers():
    return namespace_layers(NAMESPACE) + namespace_layers(REFERENCE_NAMESPACE)


def marker_layers():
    return namespace_layers(MARKER_NAMESPACE)


def identities():
    return {(screen, row['pid'], row['address']) for screen, _, row in dock_layers()}


def monitor():
    for record in query('monitors'):
        if record['name'] == OUTPUT:
            return record
    raise SafetyError(f'{OUTPUT} is not connected')


def logical_extent(record):
    width, height = record['width'] / record['scale'], record['height'] / record['scale']
    if record.get('transform', 0) % 2:
        width, height = height, width
    return width, height


def logical_size(record):
    width, height = logical_extent(record)
    return round(width), round(height)


def layout_box():
    """The bounding box of all outputs in logical pixels: what absolute pointer motion is relative to."""
    boxes = [(r['x'], r['y'], *logical_extent(r)) for r in query('monitors')]
    x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
    x1, y1 = max(b[0] + b[2] for b in boxes), max(b[1] + b[3] for b in boxes)
    return x0, y0, x1 - x0, y1 - y0


def pointer():
    position = query('cursorpos')
    return position['x'], position['y']


def app_snapshot(pins_text):
    """What the dock's row is made of: the open windows (address, class, workspace) and the pins it was given."""
    clients = sorted((c.get('address'), c.get('class'), (c.get('workspace') or {}).get('id'))
                     for c in query('clients'))
    return dict(clients=clients, pins=pins_text)


def snapshot_difference(a, b):
    parts = []
    ca, cb = set(map(tuple, a['clients'])), set(map(tuple, b['clients']))
    if ca != cb:
        parts.append(f'windows only in the Rust run {sorted(ca - cb)}, only in this run {sorted(cb - ca)}')
    if a['pins'] != b['pins']:
        parts.append('pins file differs')
    return '; '.join(parts)


def production_socket():
    # riley dock/gates/autohide_capture.py:18-20
    runtime = os.environ.get('XDG_RUNTIME_DIR', '')
    instance = os.environ.get('HYPRLAND_INSTANCE_SIGNATURE', '')
    return Path(runtime) / 'hypr' / instance / 'riley-dock.sock'


# --- The pointer: gates/pointer (riley-dock-pointer) --------------------------------------------------------------

class Hover:
    """All pointer moves of one gate run. Each hover scenario is one segment: start the helper at the cursor's
    position, move, capture with the ring hidden, then quit the helper, which puts the cursor back."""

    def __init__(self, tool, capture_dir):
        self.tool = tool
        self.enabled = tool is not None
        self.capture_dir = capture_dir
        self.stopped = None          # why the hover part stopped, if it did
        self.notices = 0             # notify-send calls in this run
        self.moves_before_notice = 0
        self.moves = 0
        self.helper = None
        self.lines = None
        self.start = None
        self.scripted = None
        self.m = None

    @property
    def active(self):
        return self.helper is not None and self.stopped is None

    def stop(self, reason):
        """Stop the hover part at once: no more moves in this run; the helper is quit now, so the cursor goes back
        (and is checked) before anything else happens."""
        if self.stopped is None:
            self.stopped = reason
        if self.m is not None:
            self.m.setdefault('hover_fault', reason)
        self.end()
        raise HoverStopped(reason)

    def notify(self):
        if self.notices:
            return
        try:
            result = subprocess.run(['notify-send', NOTICE], capture_output=True, text=True, timeout=5,
                                    stdin=subprocess.DEVNULL)
        except (OSError, subprocess.SubprocessError) as error:
            self.stop(f'notify-send failed: {error}')
        if result.returncode != 0:
            self.stop(f'notify-send failed: exit {result.returncode} {result.stderr.strip()[:200]!r}')
        self.notices += 1
        time.sleep(NOTICE_LEAD_SECONDS)

    def begin(self, m, stem):
        """Start a segment for the measurements m. Raises HoverStopped when the hover part has stopped."""
        self.m = m
        m['hover'] = True
        if self.stopped:
            m['hover_fault'] = f'hover part stopped earlier in this run: {self.stopped}'
            raise HoverStopped(self.stopped)
        if marker_layers():
            raise SafetyError(f'a {MARKER_NAMESPACE} layer is already there before the helper starts')
        self.notify()
        self.start = pointer()
        self.scripted = self.start
        m['hover_start'] = self.start
        m['hover_reads'] = 0
        x, y, w, h = layout_box()
        argv = [str(self.tool), '--restore', f'{self.start[0]:g}', f'{self.start[1]:g}', '--output', OUTPUT,
                '--layout', f'{x:g}', f'{y:g}', f'{w:g}', f'{h:g}']
        errors = (self.capture_dir / f'pointer-{stem}.stderr').open('w')
        try:
            self.helper = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors,
                                           text=True, bufsize=1, cwd=self.capture_dir)
        finally:
            errors.close()
        m['helper_pid'] = self.helper.pid
        self.lines = queue.Queue()

        def read(stream, lines):
            for line in stream:
                lines.put(line.rstrip('\n'))
            lines.put(None)
        threading.Thread(target=read, args=(self.helper.stdout, self.lines), daemon=True).start()
        self.expect('ready')
        m['marker_layers'] = sorted((screen, level) for screen, level, _ in marker_layers())

    def expect(self, word, what=None):
        try:
            line = self.lines.get(timeout=HELPER_TIMEOUT)
        except queue.Empty:
            line = f'no answer within {HELPER_TIMEOUT:g} s'
        if line != word:
            self.stop(f'pointer helper: {what or word}: {line if line is not None else "it exited"}')

    def send(self, command):
        if not self.active:
            self.stop(self.stopped or 'pointer helper not running')
        try:
            self.helper.stdin.write(command + '\n')
            self.helper.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            self.stop(f'pointer helper: {command}: {error}')
        self.expect('ok', command)

    def check(self):
        """The cursor must be where this run last put it (or where it was at the start)."""
        x, y = pointer()
        self.m['hover_reads'] += 1
        sx, sy = self.scripted
        if abs(x - sx) > POINTER_TOLERANCE or abs(y - sy) > POINTER_TOLERANCE:
            self.stop(f'pointer moved by someone else: scripted {sx:g},{sy:g}, cursor at {x:g},{y:g}')

    def move(self, x, y):
        if not self.active:
            self.stop(self.stopped or 'pointer helper not running')
        self.check()
        if not self.notices:
            self.moves_before_notice += 1
        self.send(f'move {x:.3f} {y:.3f}')
        self.moves += 1
        self.m['hover_moves'] = self.m.get('hover_moves', 0) + 1
        self.scripted = (x, y)
        self.check()

    def end(self):
        """Quit the helper (it puts the cursor back), then check the cursor and the ring's layer. Never raises."""
        if self.helper is None:
            return
        m, helper = self.m, self.helper
        try:
            if helper.poll() is None:
                try:
                    helper.stdin.write('quit\n')
                    helper.stdin.flush()
                    line = self.lines.get(timeout=HELPER_TIMEOUT)
                    m['helper_quit'] = line
                except (BrokenPipeError, OSError, queue.Empty) as error:
                    m['helper_quit'] = f'no answer ({error.__class__.__name__})'
            try:
                helper.stdin.close()
            except OSError:
                pass
            try:
                m['helper_rc'] = helper.wait(timeout=3)
            except subprocess.TimeoutExpired:
                helper.terminate()   # by its own process ID; SIGTERM also puts the cursor back
                try:
                    m['helper_rc'] = f'killed after no exit ({helper.wait(timeout=2)})'
                except subprocess.TimeoutExpired:
                    helper.kill()
                    helper.wait(timeout=2)
                    m['helper_rc'] = 'killed (SIGKILL)'
            x, y = pointer()
            m['end_pointer'] = (x, y)
            sx, sy = self.start
            m['pointer_back'] = abs(x - sx) <= POINTER_TOLERANCE and abs(y - sy) <= POINTER_TOLERANCE
            deadline = time.monotonic() + CLEANUP_SECONDS
            while True:
                left = marker_layers()
                if not left or time.monotonic() >= deadline:
                    break
                time.sleep(.05)
            m['marker_left'] = sorted((screen, level) for screen, level, _ in left)
            if not m['pointer_back'] or m['marker_left'] or m['helper_rc'] != 0:
                self.stopped = self.stopped or (f'after the helper: cursor {x:g},{y:g} (start {sx:g},{sy:g}), '
                                                f'ring layers left {m["marker_left"]}, helper exit {m["helper_rc"]}')
        except Exception as error:   # a failed query: no more moves in this run
            m['hover_end_error'] = f'{error.__class__.__name__}: {error}'
            self.stopped = self.stopped or f'could not check the cursor after the helper: {error}'
        finally:
            self.helper = None
            self.m = None


# --- Config files -------------------------------------------------------------------------------------------------

def config_xml(conf):
    """The config.xml with the settings of a riley.conf text (simple texts only: one line per key, valid values).
    The mapping is docs/files.md section 1.2; None gives None (no file)."""
    if conf is None:
        return None
    keys = {}
    for line in conf.splitlines():
        key, sep, value = line.partition('=')
        if sep:
            keys[key.strip()] = value.strip()
    out = ['<?xml version="1.0" encoding="UTF-8"?>', '<dock xmlns="urn:riley-dock:config" version="1">']
    if 'ICON_SIZE' in keys:
        out.append(f'  <icon-size>{int(keys["ICON_SIZE"])}</icon-size>')
    if 'SCREENS' in keys:
        parts = [p.strip() for p in keys['SCREENS'].split(',') if p.strip()]
        out.append('  <screens>' + ''.join(f'<screen>{escape(p)}</screen>' for p in parts) + '</screens>')
    if 'REFLECTIONS' in keys:
        out.append(f'  <reflections>{"false" if keys["REFLECTIONS"] == "0" else "true"}</reflections>')
    if 'LAUNCHER' in keys:
        v = keys['LAUNCHER']
        out.append(f'  <launcher>{v if v in ("right", "center", "off") else "left"}</launcher>')
    if 'AUTOHIDE' in keys:
        out.append(f'  <autohide>{"true" if keys["AUTOHIDE"] == "1" else "false"}</autohide>')
    out.append('</dock>')
    return '\n'.join(out) + '\n'


def xml_error_lines(text):
    return [line for line in text.splitlines()
            if line.startswith(f'{PROGRAM}: config error: ') or line.startswith(f'{PROGRAM}: style error: ')]


# --- Scenarios ----------------------------------------------------------------------------------------------------
# Each is one test dock, with the isolated config of the Rust gate it ports. "pins": None = no pins file,
# "copy" = copy the session's pins file if there is one, "text" = always write it (empty if there is none).
# "qt_config": what the Qt dock gets for the scenario's settings: "xml" = config.xml only (made by config_xml),
# "conf" = riley.conf only, "invalid" = riley.conf and INVALID_CONFIG_XML. Hyprswell always gets hyprswell.conf.
# "rust": Hyprswell (the reference) runs this scenario too. "ref": the reference scenario this one's sizes are compared with.
# "recorded": the recorded Rust size (REF information only). "hover": the scenario moves the pointer.

def scenario(name, gate, config, pins, style, args, kind, size_check=False, zone=None, recorded=None,
             qt_config='xml', rust=True, ref=None, hover=False):
    return dict(name=name, gate=gate, config=config, pins=pins, style=style, args=args, kind=kind,
                size_check=size_check, zone=zone, recorded=recorded, qt_config=qt_config, rust=rust,
                ref=ref or name, hover=hover)


REST_CONF = 'ICON_SIZE=44\nREFLECTIONS=1\n'   # riley dock/gates/reflections_capture.py:81
SCENARIOS = [
    # riley dock/gates/surface.py:25-35: empty config, cache and state; --no-swell.
    scenario('screens', 'surface.py', None, None, False, ['--no-swell'], 'screens'),
    # riley dock/gates/reflections_capture.py:81-85,95, with its hover (:92-93, :109-121).
    scenario('rest', 'reflections_capture.py', REST_CONF, 'copy', True, [], 'rest', True, REST_ZONE, REST_SIZE,
             hover=True),
    # The same with reflections off; checked against 'rest' by the Rust layout rule (riley dock/src/autohide.rs:34-45).
    scenario('reflections-off', 'reflections_capture.py', 'ICON_SIZE=44\nREFLECTIONS=0\n', 'copy', True, [],
             'reflections-off'),
    # docs/files.md section 1.1: with no config.xml the Qt dock reads riley.conf, so 'rest' from riley.conf alone.
    scenario('conf-fallback', 'reflections_capture.py', REST_CONF, 'copy', True, [], 'rest', True, REST_ZONE,
             REST_SIZE, qt_config='conf', rust=False, ref='rest'),
    # docs/files.md sections 1.1 and 4: a config.xml that fails its schema is not used at all; one error line, and
    # the riley.conf settings ('rest').
    scenario('invalid-config', 'reflections_capture.py', REST_CONF, 'copy', True, [], 'rest', True, REST_ZONE,
             REST_SIZE, qt_config='invalid', rust=False, ref='rest'),
] + [
    # riley dock/gates/launcher_capture.py:32-41,49
    scenario(f'launcher-{mode}', 'launcher_capture.py', f'ICON_SIZE=44\nREFLECTIONS=1\nLAUNCHER={mode}\n', 'text',
             True, [], 'rest', True, REST_ZONE, LAUNCHER_OFF_SIZE if mode == 'off' else REST_SIZE)
    for mode in ('left', 'right', 'center', 'off')
] + [
    # riley dock/gates/autohide_capture.py:36-41,51: one dock, two scenarios (hidden, then shown after --show), with
    # its hover (:49, :87-90).
    scenario('autohide', 'autohide_capture.py', 'ICON_SIZE=44\nREFLECTIONS=1\nAUTOHIDE=1\n', 'copy', True,
             ['--no-swell'], 'autohide', True, HIDDEN_ZONE, AUTOHIDE_SIZE, hover=True),
]


# --- One test dock ------------------------------------------------------------------------------------------------

def settle(child, pid):
    """riley dock/gates/reflections_capture.py:48-66: wait for a non-placeholder layer, unchanged for one second."""
    deadline = time.monotonic() + SETTLE_SECONDS
    last, since = None, time.monotonic()
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise Missing(f'dock exited ({child.returncode}) while settling')
        rows = [row for _, _, row in dock_layers() if row.get('pid') == pid]
        if len(rows) != 1:
            raise Missing(f'{len(rows)} dock layers while settling')
        row, reserved = rows[0], monitor()['reserved']
        signature = (row['x'], row['y'], row['w'], row['h'], tuple(reserved))
        ready = row['w'] > 0 and 0 < row['h'] <= MAX_SETTLED_HEIGHT
        if not ready or signature != last:
            last, since = signature, time.monotonic()
        elif time.monotonic() - since >= QUIET_SECONDS:
            return dict(x=row['x'], y=row['y'], w=row['w'], h=row['h']), list(reserved), round(time.monotonic() - since, 3)
        time.sleep(.1)
    raise Missing(f'not settled within {SETTLE_SECONDS:.0f} s, last {last}')


def owned_row(pid):
    rows = [(screen, row) for screen, _, row in dock_layers() if row.get('pid') == pid]
    if len(rows) != 1 or rows[0][0] != OUTPUT:
        raise Missing(f'expected one test dock layer on {OUTPUT}, found {[s for s, _ in rows]}')
    row = rows[0][1]
    return dict(x=row['x'], y=row['y'], w=row['w'], h=row['h'])


def capture(pid, path, scale, hover):
    """riley dock/gates/reflections_capture.py:39-45: the whole surface at DP-2's scale. While the helper runs, its
    ring is hidden for the screenshot (and the cursor checked first)."""
    row = owned_row(pid)
    geometry = f'{row["x"]},{row["y"]} {row["w"]}x{row["h"]}'
    ring = hover.active
    if ring:
        hover.check()
        hover.send('hide')
    try:
        subprocess.run(['grim', '-s', str(scale), '-g', geometry, str(path)], check=True, capture_output=True,
                       timeout=3)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise Missing(f'screenshot failed: {error}') from error
    finally:
        if ring and hover.active:
            hover.send('show')
    png = path.read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
    x, y = pointer()
    outside = not (row['x'] <= x < row['x'] + row['w'] and row['y'] <= y < row['y'] + row['h'])
    return row, png, outside


def stop(child):
    # By the child's own process ID only (riley dock/gates/launcher_capture.py:13-20).
    if child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=2)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=2)


def write_isolated(home, scn, label):
    config, cache, state = home / 'config', home / 'cache', home / 'state'
    for folder in (config, cache, state):
        folder.mkdir()
    if scn['config'] is not None:
        if label == 'rust':
            (config / 'nwg-dock-hyprland').mkdir()
            (config / REFERENCE_CONFIG).write_text(scn['config'])
        elif scn['qt_config'] in ('conf', 'invalid'):
            (config / 'nwg-dock-hyprland').mkdir()
            (config / 'nwg-dock-hyprland/riley.conf').write_text(scn['config'])
        if label == 'qt' and scn['qt_config'] in ('xml', 'invalid'):
            (config / 'riley-dock').mkdir(exist_ok=True)
            text = INVALID_CONFIG_XML if scn['qt_config'] == 'invalid' else config_xml(scn['config'])
            (config / 'riley-dock/config.xml').write_text(text)
    if scn['style']:
        # The Rust gates copy riley dock/style.example.css. Hyprswell builds in the same file, byte for byte, as its
        # fallback for a missing hyprswell/style.css (hyprswell src/ui.rs:282-285), so it gets the same style without
        # the copy; the Qt dock gets the XML file with the same values (docs/files.md, section 3.2).
        if label == 'qt':
            (config / 'riley-dock').mkdir(exist_ok=True)
            shutil.copyfile(ROOT / 'style.example.xml', config / 'riley-dock/style.xml')
    real_pins = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'nwg-dock-pinned'
    pins_text = None
    if scn['pins'] == 'copy' and real_pins.exists():
        shutil.copyfile(real_pins, cache / 'nwg-dock-pinned')
    elif scn['pins'] == 'text':
        (cache / 'nwg-dock-pinned').write_text(real_pins.read_text() if real_pins.exists() else '')
    if (cache / 'nwg-dock-pinned').exists():
        pins_text = (cache / 'nwg-dock-pinned').read_bytes()
    env = os.environ.copy()
    env.update(HOME=str(home), XDG_CONFIG_HOME=str(config), XDG_CACHE_HOME=str(cache), XDG_STATE_HOME=str(state),
               XDG_DATA_HOME=os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share')))
    for key, real in (('HOME', Path.home()),
                      ('XDG_CONFIG_HOME', Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config'))),
                      ('XDG_CACHE_HOME', Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache'))),
                      ('XDG_STATE_HOME', Path(os.environ.get('XDG_STATE_HOME', Path.home() / '.local/state')))):
        if Path(env[key]).resolve() == real.resolve() or not Path(env[key]).resolve().is_relative_to(home.resolve()):
            raise SafetyError(f'{key} is not isolated')
    return env, pins_text


def measure(binary, scn, label, capture_dir, hover):
    """Run one test dock for one scenario and return what was measured. Raises SafetyError to stop the gate."""
    m = {}
    mon = monitor()
    baseline = identities()
    if any(screen == OUTPUT for screen, _, _ in baseline):
        raise SafetyError(f'{OUTPUT} already has a dock layer; leaving it alone')
    before = list(mon['reserved'])
    m.update(before=before, monitor=dict(x=mon['x'], y=mon['y'], size=logical_size(mon)), scale=mon['scale'])
    if not (binary and os.access(binary, os.X_OK) and Path(binary).is_file()):
        m['why'] = f'not started: binary missing ({binary})'
        return m
    child = None
    with tempfile.TemporaryDirectory(prefix='home-', dir=capture_dir) as directory:
        home = Path(directory)
        env, pins_text = write_isolated(home, scn, label)
        m['config_xml_path'] = str(home / 'config/riley-dock/config.xml')
        socket = home / 'control.sock'
        if socket.resolve() == production_socket().resolve() or len(str(socket)) > 100:
            raise SafetyError('the private control socket is not usable')
        m['socket_private'] = True
        diagnostics = DIAGNOSTICS_SURFACE if scn['kind'] == 'screens' else DIAGNOSTICS
        errors_path = home / 'stderr'
        try:
            if hover.enabled and scn['hover']:
                try:
                    hover.begin(m, f'{scn["name"]}-{label}')
                    hover.move(mon['x'] + 2, mon['y'] + 80)   # riley dock/gates/reflections_capture.py:92-93
                except HoverStopped:
                    pass
            m['apps'] = app_snapshot(pins_text)   # just before the dock starts (Marc's decision 2026-11-12)
            with errors_path.open('w+') as errors:
                argv = [str(binary), '--output', OUTPUT, *scn['args'], '--control-socket', str(socket)]
                started = time.monotonic()
                child = subprocess.Popen(argv, env=env, stdout=errors, stderr=errors, stdin=subprocess.DEVNULL,
                                         cwd=home)
                m['pid'] = child.pid
                try:
                    run_dock(child, scn, m, started, binary, env, socket, capture_dir, label, hover)
                except Missing as error:
                    m['why'] = str(error)
                finally:
                    stop(child)
                    errors.flush()
                    errors.seek(0)
                    text = errors.read()
                    m['diagnostics'] = [word for word in diagnostics if word in text]
                    m['xml_errors'] = xml_error_lines(text)
                    if text.strip():
                        (capture_dir / f'{scn["name"]}-{label}.stderr').write_text(text[-20000:])
            log = home / 'state/riley/dock.log'
            m['log_xml_errors'] = xml_error_lines(log.read_text(errors='replace')) if log.exists() else []
        finally:
            hover.end()   # puts the cursor back after the dock is stopped, as reflections_capture.py:133-141
        if child is not None:
            deadline = time.monotonic() + CLEANUP_SECONDS
            gone = False
            while time.monotonic() < deadline:
                if not any(row.get('pid') == child.pid for _, _, row in dock_layers()):
                    gone = True
                    break
                time.sleep(.05)
            m['gone'] = gone
            m['socket_removed'] = not socket.exists() and not socket.is_symlink()
    deadline = time.monotonic() + CLEANUP_SECONDS
    while True:
        restored = list(monitor()['reserved'])
        if restored == before or time.monotonic() >= deadline:
            break
        time.sleep(.05)
    m['restored'] = restored
    m['identities_same'] = identities() == baseline
    if child is not None and not (m.get('gone') and restored == before and m['identities_same']):
        m['unsafe'] = (f'after the test dock: layer gone={m.get("gone")}, reserved {restored} (before {before}), '
                       f'session docks unchanged={m["identities_same"]}')
    return m


def run_dock(child, scn, m, started, binary, env, socket, capture_dir, label, hover):
    pid = child.pid
    baseline_addresses = {row['address'] for _, _, row in dock_layers() if row.get('pid') != pid}
    deadline = started + APPEAR_SECONDS
    found = None
    while found is None:
        if child.poll() is not None:
            raise Missing(f'dock exited ({child.returncode}) before its layer appeared')
        rows = [(screen, level, row) for screen, level, row in dock_layers() if row.get('pid') == pid]
        if rows:
            found = rows
            m['appeared'] = round(time.monotonic() - started, 3)
            break
        if time.monotonic() >= deadline:
            raise Missing(f'no layer within {APPEAR_SECONDS:.0f} s')
        time.sleep(.05)
    m['outputs'] = sorted(screen for screen, _, row in dock_layers()
                          if row['address'] not in baseline_addresses)
    m['level'] = found[0][1]
    m['first_size'] = (found[0][2]['w'], found[0][2]['h'])
    settled, reserved, _ = settle(child, pid)
    m['settled'], m['reserved'] = settled, reserved
    pose = 'hidden' if scn['kind'] == 'autohide' else None
    path = capture_dir / (f'{scn["name"]}-{pose}-{label}.png' if pose else f'{scn["name"]}-{label}.png')
    try:
        row, png, outside = capture(pid, path, m['scale'], hover)
    except HoverStopped:
        hover_off(m)
        row, png, outside = capture(pid, path, m['scale'], hover)
    m.update(capture_row=row, png=png, pointer_outside=outside, png_path=str(path))
    if scn['name'] == 'rest' and hover.active:
        # riley dock/gates/reflections_capture.py:109-121: one entry into the visible bar, then the swollen surface.
        try:
            zone = reserved[3] - m['before'][3]
            hover.move(row['x'] + row['w'] / 2, row['y'] + row['h'] - 8 - (zone - 8) / 2)
            time.sleep(SWELL_SECONDS)
            m['hover_alive'] = child.poll() is None
            if not m['hover_alive']:
                raise Missing('dock exited during hover')
            grown, m['swollen_png'], _ = capture(pid, capture_dir / f'swollen-{label}.png', m['scale'], hover)
            m['swollen_size'] = (grown['w'], grown['h'])
            m['swollen_reserved'] = list(monitor()['reserved'])
            hover.check()
        except HoverStopped:
            hover_off(m)
    if scn['kind'] == 'autohide':
        show(child, m, binary, env, socket, capture_dir, label, hover)


def hover_off(m):
    """The hover part stopped during this dock's run: its remaining captures are taken without the pointer."""
    m.setdefault('hover_fault', 'hover part stopped')


def show(child, m, binary, env, socket, capture_dir, label, hover):
    """riley dock/gates/autohide_capture.py:65-95; the pointer is moved into the bar only by the hover part."""
    samples, done = [], threading.Event()

    def sample():
        while not done.is_set():
            try:
                samples.append(tuple(monitor()['reserved']))
            except Exception as error:   # a failed query is a sample that cannot match
                samples.append(('query failed', str(error)))
            time.sleep(SAMPLE_SECONDS)

    sampler = threading.Thread(target=sample, daemon=True)
    sampler.start()
    started = time.monotonic()
    try:
        shown = subprocess.run([str(binary), '--show', '--control-socket', str(socket)], env=env, cwd=socket.parent,
                               capture_output=True, text=True, timeout=SHOW_TIMEOUT, stdin=subprocess.DEVNULL)
        m['show_rc'] = shown.returncode
        m['show_stderr'] = shown.stderr.strip()[:200]
    except subprocess.TimeoutExpired:
        m['show_rc'] = f'no answer within {SHOW_TIMEOUT} s'
    finally:
        done.set()
        sampler.join()
    m['show_seconds'] = round(time.monotonic() - started, 3)
    m['samples'] = sorted(set(samples), key=str)
    m['sample_count'] = len(samples)
    m['moved'] = owned_row(child.pid)
    held = False
    if hover.active:
        try:
            moved = m['moved']
            # riley dock/gates/autohide_capture.py:87-89: into the reflection lane below the icons, held 1 s.
            hover.move(moved['x'] + moved['w'] / 2, moved['y'] + moved['h'] - 14)
            time.sleep(SHOWN_HOLD_SECONDS)
            m['alive'] = child.poll() is None
            held = True
        except HoverStopped:
            hover_off(m)
    # Without the pointer to hold it open the dock hides 700 ms after the reveal, so the shown pose is taken at once.
    path = capture_dir / f'autohide-shown-{label}.png'
    try:
        _, m['shown_png'], m['shown_pointer_outside'] = capture(child.pid, path, m['scale'], hover)
    except HoverStopped:
        hover_off(m)
        _, m['shown_png'], m['shown_pointer_outside'] = capture(child.pid, path, m['scale'], hover)
    m['shown_reserved'] = list(monitor()['reserved'])
    if hover.active:
        try:
            hover.check()
        except HoverStopped:
            hover_off(m)
    if not held:
        time.sleep(SHOWN_HOLD_SECONDS)
        m['alive'] = child.poll() is None


# --- Checks -------------------------------------------------------------------------------------------------------

Check = namedtuple('Check', 'scenario what fn expected source qt_only', defaults=(False,))


def need(m, key):
    if m.get(key) is None:
        raise Missing(m.get('why') or f'{key} not measured')
    return m[key]


def need_hover(m, key):
    """A result of the hover part itself: not valid once that part stopped."""
    if m.get('hover_fault'):
        raise Missing(m['hover_fault'])
    return need(m, key)


def need_segment(m, key):
    """A result of starting or ending the helper: valid whenever it was measured."""
    if m.get(key) is None:
        raise Missing(m.get('hover_fault') or m.get('why') or f'{key} not measured')
    return m[key]


def size_text(size):
    return f'{size[0]}x{size[1]}'


def expected_position(m, row):
    mx, my = m['monitor']['x'], m['monitor']['y']
    width, height = m['monitor']['size']
    return mx + (width - row['w']) / 2, my + height - row['h']


def zone_text(m, reserved):
    return f'bottom {reserved[3] - m["before"][3]} {reserved}'


def yes(value):
    return 'yes' if value else 'no'


def checks_for(scn, hover_on):
    """The checks of one scenario, as Check(scenario, what, function, expected, source, qt_only).

    A function takes the measurements of this scenario and a context (ctx['own'], the same dock's other scenarios;
    ctx['rust'], Hyprswell's scenarios of this run; ctx['have_rust']), and returns (ok, actual) or
    (ok, actual, expected) when the expected text depends on the session (DP-2's reserved area before the run, for
    example). A value that was not measured makes the check fail. A qt_only check is not evaluated for the Rust
    dock (it compares with Hyprswell)."""
    name, gate, kind = scn['name'], scn['gate'], scn['kind']
    rc = 'riley dock/gates/reflections_capture.py'
    ah = 'riley dock/gates/autohide_capture.py'
    src = {   # where each Rust capture gate makes the same check
        'surface.py': dict(appear='surface.py:39-56', layers='surface.py:44-49', settle='reflections_capture.py:48-66',
                           same='reflections_capture.py:108', png='reflections_capture.py:39-45',
                           diag='surface.py:61', gone='surface.py:70-73', restored='reflections_capture.py:147',
                           ids='reflections_capture.py:148'),
        'reflections_capture.py': dict(appear='reflections_capture.py:99-103', layers='reflections_capture.py:33-36',
                                       settle='reflections_capture.py:48-66', same='reflections_capture.py:108',
                                       png='reflections_capture.py:39-45', diag='reflections_capture.py:125',
                                       gone='reflections_capture.py:143-146', restored='reflections_capture.py:147',
                                       ids='reflections_capture.py:148'),
        'launcher_capture.py': dict(appear='launcher_capture.py:53-57', layers='reflections_capture.py:33-36',
                                    settle='launcher_capture.py:58', same='launcher_capture.py:62',
                                    png='launcher_capture.py:59-60', diag='launcher_capture.py:63-65',
                                    gone='launcher_capture.py:71-75', restored='launcher_capture.py:77',
                                    ids='launcher_capture.py:78'),
        'autohide_capture.py': dict(appear='autohide_capture.py:55-59', layers='reflections_capture.py:33-36',
                                    settle='autohide_capture.py:60', same='autohide_capture.py:63-64',
                                    png='autohide_capture.py:63', diag='autohide_capture.py:96-99',
                                    gone='autohide_capture.py:111-114', restored='autohide_capture.py:117-119',
                                    ids='autohide_capture.py:120'),
    }[gate]
    src = {key: f'riley dock/gates/{value}' for key, value in src.items()}
    diagnostics = DIAGNOSTICS_SURFACE if kind == 'screens' else DIAGNOSTICS
    hovering = hover_on and scn['hover']
    out = []

    def add(scenario_name, what, fn, expected, source, qt_only=False):
        out.append(Check(scenario_name, what, fn, expected, source, qt_only))

    first = f'{name}-hidden' if kind == 'autohide' else name
    if kind == 'autohide':
        add(first, 'private control socket', lambda m, _: (need(m, 'socket_private'), yes(m['socket_private'])),
            'yes, not the session\'s socket', f'{ah}:41-42')
    if hovering:
        add(first, 'ring layer while the pointer moves',
            lambda m, _: (need_segment(m, 'marker_layers') == [(OUTPUT, OVERLAY_LEVEL)], str(m['marker_layers'])),
            f"[('{OUTPUT}', '{OVERLAY_LEVEL}')]: one {MARKER_NAMESPACE} layer on {OUTPUT}, overlay", SRC_CURSOR)
    add(first, 'layer appeared',
        lambda m, _: (need(m, 'appeared') <= APPEAR_SECONDS, f'after {m["appeared"]} s'),
        f'within {APPEAR_SECONDS:g} s', src['appear'])
    add(first, 'dock layers',
        lambda m, _: (need(m, 'outputs') == [OUTPUT], f'{len(m["outputs"])} on {m["outputs"]}'),
        f"1 on ['{OUTPUT}']", src['layers'])
    add(first, 'layer level', lambda m, _: (need(m, 'level') == TOP_LEVEL, m['level']), f'{TOP_LEVEL} (TOP)',
        'riley dock/gates/surface.py:51')
    if kind == 'screens':
        add(first, 'first surface size',
            lambda m, _: (min(need(m, 'first_size')) > 0, size_text(m['first_size'])), 'w > 0 and h > 0',
            'riley dock/gates/surface.py:52')
    add(first, 'settled surface', lambda m, _: (True, size_text((need(m, 'settled')['w'], m['settled']['h']))),
        f'0 < h <= {MAX_SETTLED_HEIGHT}, unchanged for {QUIET_SECONDS:g} s within {SETTLE_SECONDS:g} s',
        src['settle'])
    if scn['size_check']:
        def size_vs_rust(m, ctx, ref=scn['ref']):
            if not ctx['have_rust']:
                raise Missing('needs HYPRSWELL')
            rust = ctx['rust'].get(ref) or {}
            if not rust.get('settled'):
                raise Missing(f'Hyprswell\'s {ref} run measured no surface ({rust.get("why", "not run")})')
            r, q = rust['settled'], need(m, 'settled')
            expected = f'{size_text((r["w"], r["h"]))} (Hyprswell, {ref}, this run)'
            if need(m, 'apps') != need(rust, 'apps'):
                return (False, f'{size_text((q["w"], q["h"]))}; app set changed between runs '
                               f'({snapshot_difference(rust["apps"], m["apps"])})', expected)
            return (q['w'], q['h']) == (r['w'], r['h']), size_text((q['w'], q['h'])), expected
        add(first, 'surface size', size_vs_rust, f'as Hyprswell\'s {scn["ref"]} in this run', SRC_SAME_RUN,
            qt_only=True)

    def position(m, _):
        row = need(m, 'settled')
        ex, ey = expected_position(m, row)
        return (abs(row['x'] - ex) <= 0.5 and row['y'] == ey, f'{row["x"]},{row["y"]}',
                f'{ex:g},{ey:g} (centred, bottom edge on the bottom of {OUTPUT})')
    add(first, 'surface position', position, f'centred, bottom edge on the bottom of {OUTPUT}', SRC_ANCHOR)
    if scn['zone'] is not None:
        def zone_check(m, _, z=scn['zone']):
            reserved, before = need(m, 'reserved'), m['before']
            expected = before[:3] + [before[3] + z]
            return reserved == expected, zone_text(m, reserved), f'bottom {z} {expected}'
        if kind == 'autohide':
            add(first, 'hidden reservation', zone_check, f'bottom {scn["zone"]}', f'{ah}:61-62')
        else:
            add(first, 'reservation', zone_check, f'bottom {scn["zone"]}', SRC_RECORDED)
    if kind == 'reflections-off':
        # Reflections add the same lane to the bar, so to the reservation, and to the surface height
        # (riley dock/src/autohide.rs:36-44, motion.rs:154-161); they never change the surface width (autohide.rs:39).
        def lane(m, ctx):
            rest = ctx['own'].get('rest') or {}
            r, o = need(rest, 'settled'), need(m, 'settled')
            dz = rest['reserved'][3] - m['reserved'][3]
            dh = r['h'] - o['h']
            return dz == dh and dz > 0, f'surface {dh} lower, reservation {dz} lower than rest'
        add(first, 'reflection lane vs rest', lane, 'surface and reservation lower by the same amount, above 0',
            'riley dock/src/autohide.rs:36-44, dock/src/motion.rs:154-161')

        def width(m, ctx):
            rest = ctx['own'].get('rest') or {}
            return (need(m, 'settled')['w'] == need(rest, 'settled')['w'], str(m['settled']['w']),
                    f'{rest["settled"]["w"]} (as rest)')
        add(first, 'surface width vs rest', width, 'as rest', 'riley dock/src/autohide.rs:39')
    add(first, 'layout at capture',
        lambda m, _: (need(m, 'capture_row') == need(m, 'settled'), f'{m["capture_row"]}'),
        'unchanged since settled', src['same'])
    add(first, 'pointer outside the dock', lambda m, _: (need(m, 'pointer_outside'), yes(m['pointer_outside'])),
        'yes', f'{rc}:92-93' + ('' if hovering else ' (there the pointer is moved out; here it is only read)'))
    add(first, 'screenshot', lambda m, _: (need(m, 'png'), 'PNG' if m['png'] else 'not a PNG'), 'PNG', src['png'])
    if name == 'rest' and hovering:
        add(first, 'dock alive during hover', lambda m, _: (need_hover(m, 'hover_alive'), yes(m['hover_alive'])),
            'yes', f'{rc}:114')
        add(first, 'swell screenshot',
            lambda m, _: (need_hover(m, 'swollen_png'), 'PNG' if m['swollen_png'] else 'not a PNG'), 'PNG',
            f'{rc}:115, :44')
        add(first, 'reservation during swell',
            lambda m, _: (need_hover(m, 'swollen_reserved') == m['reserved'], str(m['swollen_reserved'])),
            'as at rest', f'{rc}:116-120')
        add(first, 'surface during swell',
            lambda m, _: (need_hover(m, 'swollen_size') == (m['settled']['w'], m['settled']['h']),
                          size_text(m['swollen_size'])), 'as at rest', f'{rc}:115,121')
    last = name
    if kind == 'autohide':
        last = f'{name}-shown'
        add(last, 'show request', lambda m, _: (need(m, 'show_rc') == 0, f'exit {m["show_rc"]} after {m.get("show_seconds")} s'),
            f'exit 0 within {SHOW_TIMEOUT} s', f'{ah}:76-83')
        add(last, 'reservation during reveal',
            lambda m, _: (need(m, 'sample_count') > 0 and m['samples'] == [tuple(m['reserved'])],
                          f'{m["sample_count"]} samples, values {m["samples"]}', f'only {tuple(m["reserved"])} (hidden)'),
            'only the hidden reservation', f'{ah}:65-84')
        add(last, 'surface after show', lambda m, _: (need(m, 'moved') == m['settled'], f'{m["moved"]}'),
            'unchanged since hidden', f'{ah}:85-86')
        add(last, 'screenshot', lambda m, _: (need(m, 'shown_png'), 'PNG' if m['shown_png'] else 'not a PNG'), 'PNG',
            f'{ah}:92')
        add(last, 'shown reservation',
            lambda m, _: (need(m, 'shown_reserved') == m['reserved'], zone_text(m, m['shown_reserved']),
                          f'{zone_text(m, m["reserved"])} (as hidden)'), 'as hidden', f'{ah}:93-95')
        add(last, 'dock alive after show', lambda m, _: (need(m, 'alive'), yes(m['alive'])), 'yes', f'{ah}:90-91')
    if hovering:
        def scripted(m, _):
            moves, reads = need_hover(m, 'hover_moves'), need_hover(m, 'hover_reads')
            return True, f'{moves} moves, {reads} cursor reads, all within {POINTER_TOLERANCE:g} px'
        add(last, 'cursor at each scripted position', scripted,
            f'within {POINTER_TOLERANCE:g} px after each move and before each screenshot, else "pointer moved by '
            'someone else"', SRC_CURSOR)
    if scn['qt_config'] == 'invalid':
        def error_line(m, _, key):
            lines = need(m, key)
            expected = f'{PROGRAM}: config error: {m["config_xml_path"]}:{INVALID_CONFIG_MESSAGE}'
            return lines == [expected], f'{len(lines)} line(s) {lines}', f'exactly [{expected!r}]'
        add(last, 'config error line on stderr', lambda m, c: error_line(m, c, 'xml_errors'),
            'exactly one: config.xml:4, the launcher facet message', SRC_XML_ERRORS, qt_only=True)
        add(last, 'config error line in dock.log', lambda m, c: error_line(m, c, 'log_xml_errors'),
            'exactly one, the same line', f'{SRC_XML_ERRORS} ("the same line to its log")', qt_only=True)
    else:
        add(last, 'config and style error lines',
            lambda m, _: (need(m, 'xml_errors') == [], f'{len(m["xml_errors"])} {m["xml_errors"]}'),
            'none (every file the scenario writes is valid)', f'{SRC_XML_ERRORS}; docs/files.md section 1.1',
            qt_only=True)
    add(last, 'stderr diagnostics',
        lambda m, _: (need(m, 'diagnostics') == [], ', '.join(m['diagnostics']) or 'none'),
        'none of ' + ', '.join(diagnostics), src['diag'])
    add(last, 'layer gone after stop by PID', lambda m, _: (need(m, 'gone'), yes(m['gone'])),
        f'yes, within {CLEANUP_SECONDS:g} s', src['gone'])
    if kind == 'autohide':
        add(last, 'private control socket removed', lambda m, _: (need(m, 'socket_removed'), yes(m['socket_removed'])),
            'yes', f'{ah}:115')
    if hovering:
        add(last, 'pointer helper quit',
            lambda m, _: (need_segment(m, 'helper_quit') == 'ok' and m.get('helper_rc') == 0,
                          f'answer {m["helper_quit"]!r}, exit {m.get("helper_rc")}'),
            "answer 'ok', exit 0", 'gates/pointer/main.cpp (quit)')
        add(last, 'cursor back at the start',
            lambda m, _: (need_segment(m, 'pointer_back'), f'{m["end_pointer"]} (start {m["hover_start"]})',
                          f'{m["hover_start"]} within {POINTER_TOLERANCE:g} px'),
            f'the position before the first move, within {POINTER_TOLERANCE:g} px',
            f'{SRC_CURSOR}; {rc}:141, {ah}:109')
        add(last, 'ring layer gone',
            lambda m, _: (need_segment(m, 'marker_left') == [], str(m['marker_left'])),
            f'no {MARKER_NAMESPACE} layer within {CLEANUP_SECONDS:g} s', SRC_CURSOR)
    add(last, f'{OUTPUT} reserved after cleanup',
        lambda m, _: (need(m, 'restored') == m['before'], str(m['restored']), f'{m["before"]} (as before)'),
        'as before the run', src['restored'])
    add(last, 'session docks unchanged', lambda m, _: (need(m, 'identities_same'), yes(m['identities_same'])),
        'yes', src['ids'])
    return out


def gate_checks(hover):
    """Checks of the whole run, not of one scenario."""
    if not hover.enabled:
        return []
    return [Check('gate', 'notification before the first move',
                  lambda m, _: (hover.notices == 1 and hover.moves_before_notice == 0,
                                f'{hover.notices} notification(s), {hover.moves_before_notice} move(s) before it, '
                                f'{hover.moves} move(s) in all'),
                  f"exactly one notify-send '{NOTICE}' before the first move", SRC_CURSOR)]


def evaluate(check, m, ctx):
    """(ok, actual, expected) for one check on one dock's measurements."""
    try:
        result = check.fn(m, ctx)
    except Missing as error:
        return False, f'not measured ({error})', check.expected
    except (KeyError, TypeError, IndexError) as error:
        return False, f'not measured ({error.__class__.__name__}: {error})', check.expected
    return result[0], result[1], result[2] if len(result) > 2 else check.expected


def summary(m):
    if m.get('why') and not m.get('settled'):
        return m['why']
    parts = []
    if m.get('settled'):
        s = m['settled']
        parts.append(f'surface {s["w"]}x{s["h"]} at {s["x"]},{s["y"]}')
    if m.get('reserved'):
        parts.append(f'reserved {zone_text(m, m["reserved"])}')
    if m.get('level') is not None:
        parts.append(f'level {m["level"]}, appeared after {m.get("appeared")} s')
    if m.get('apps'):
        parts.append(f'{len(m["apps"]["clients"])} windows open')
    if 'show_rc' in m:
        parts.append(f'show exit {m["show_rc"]} after {m.get("show_seconds")} s, {m.get("sample_count")} samples '
                     f'{m.get("samples")}, shown reserved {m.get("shown_reserved")}')
    if m.get('swollen_size'):
        parts.append(f'swollen {size_text(m["swollen_size"])}')
    if m.get('hover_fault'):
        parts.append(f'hover: {m["hover_fault"]}')
    if m.get('why'):
        parts.append(f'stopped: {m["why"]}')
    return '; '.join(parts) or 'nothing measured'


def side_by_side(rust, qt, side):
    if not (rust.exists() and qt.exists()):
        return False
    if shutil.which('magick'):
        subprocess.run(['magick', str(rust), str(qt), '-background', 'none', '+append', str(side)],
                       check=True, capture_output=True, timeout=20)
        return True
    try:
        from PIL import Image
    except ImportError:
        return False
    left, right = Image.open(rust), Image.open(qt)
    out = Image.new('RGBA', (left.width + right.width, max(left.height, right.height)), (0, 0, 0, 0))
    out.paste(left, (0, 0))
    out.paste(right, (left.width, 0))
    out.save(side)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--qt-dock', required=True)
    parser.add_argument('--hyprswell')
    parser.add_argument('--capture-dir', required=True)
    parser.add_argument('--pointer-tool', help='riley-dock-pointer (gates/pointer), needed unless --no-hover')
    parser.add_argument('--no-hover', action='store_true',
                        help='leave the pointer alone: skip the swell-under-hover and held-open captures')
    options = parser.parse_args()
    capture_dir = Path(options.capture_dir).resolve()
    capture_dir.mkdir(parents=True, exist_ok=True)
    print(f'qt dock: {options.qt_dock}')
    print(f'reference dock: {options.hyprswell or "none (the surface size checks need HYPRSWELL)"}')
    print(f'captures: {capture_dir}')
    tool = None
    if not options.no_hover:
        tool = options.pointer_tool
        if not (tool and os.access(tool, os.X_OK) and Path(tool).is_file()):
            print(f'GATE FAIL pointer helper missing ({tool}); build riley-dock-pointer or pass --no-hover')
            return 1
        print(f'HOVER: this run moves the cursor on {OUTPUT} with {tool} (red ring, one notification first)')
    hover = Hover(tool, capture_dir)
    have_rust = bool(options.hyprswell)

    all_checks = [(scn, checks_for(scn, hover.enabled)) for scn in SCENARIOS]
    final_checks = gate_checks(hover)
    total = sum(len(checks) for _, checks in all_checks) + len(final_checks)
    measures = {'qt': {}, 'rust': {}}
    stopped = None
    failed, passed = [], 0

    def context(label):
        return dict(own=measures[label], rust=measures['rust'], have_rust=have_rust)

    for scn, checks in all_checks:
        docks = ([('rust', options.hyprswell)] if have_rust and scn['rust'] else []) + [('qt', options.qt_dock)]
        for label, binary in docks:
            if stopped:
                measures[label][scn['name']] = {'why': f'not run: stopped after a safety failure ({stopped})'}
                continue
            try:
                m = measure(binary, scn, label, capture_dir, hover)
            except SafetyError as error:
                stopped = str(error)
                print(f'FAIL safety: {error}', flush=True)
                m = {'why': f'not run: safety failure ({error})'}
            except (subprocess.SubprocessError, OSError, json.JSONDecodeError) as error:
                stopped = f'{error.__class__.__name__}: {error}'
                print(f'FAIL safety: could not run or read the session: {stopped}', flush=True)
                m = {'why': f'not run: {stopped}'}
            measures[label][scn['name']] = m
            if m.get('unsafe'):
                stopped = m['unsafe']
                print(f'FAIL safety: {m["unsafe"]}', flush=True)
            if label == 'rust':
                print(f'REF {scn["name"]}: hyprswell measured {summary(m)}', flush=True)
                for check in checks:
                    if check.qt_only:
                        continue
                    ok, actual, expected = evaluate(check, m, context('rust'))
                    if not ok:
                        print(f'REF MISMATCH {check.scenario}: {check.what} = {actual}, expected {expected} '
                              f'({check.source})', flush=True)
        m = measures['qt'][scn['name']]
        if scn['recorded']:
            now = []
            for label, key in (('rust', scn['ref']), ('qt', scn['name'])):
                s = (measures[label].get(key) or {}).get('settled')
                if s:
                    now.append(f'{label} now {size_text((s["w"], s["h"]))}')
            print(f'REF {scn["name"]}: recorded surface {size_text(scn["recorded"])} ({SRC_RECORDED}); '
                  + (', '.join(now) or 'nothing measured now'), flush=True)
        for check in checks:
            ok, actual, expected = evaluate(check, m, context('qt'))
            if ok:
                passed += 1
                print(f'PASS {check.scenario}: {check.what} = {actual}', flush=True)
            else:
                failed.append(check.scenario)
                print(f'FAIL {check.scenario}: {check.what} = {actual}, expected {expected} ({check.source})',
                      flush=True)
        stems = [(f'{scn["name"]}-{pose}', f'{scn["ref"]}-{pose}') for pose in ('hidden', 'shown')] \
            if scn['kind'] == 'autohide' else [(scn['name'], scn['ref'])]
        if scn['name'] == 'rest' and hover.enabled:
            stems.append(('swollen', 'swollen'))
        for stem, rust_stem in stems:
            qt_png, rust_png = capture_dir / f'{stem}-qt.png', capture_dir / f'{rust_stem}-rust.png'
            side = capture_dir / f'{stem}-side.png'
            if have_rust and side_by_side(rust_png, qt_png, side):
                print(f'IMAGES {stem}: {side}')
            else:
                shots = [str(p) for p in (rust_png, qt_png) if p.exists()]
                print(f'IMAGES {stem}: {", ".join(shots) or "none"}')
    for check in final_checks:
        ok, actual, expected = evaluate(check, {}, context('qt'))
        if ok:
            passed += 1
            print(f'PASS {check.scenario}: {check.what} = {actual}', flush=True)
        else:
            failed.append(check.scenario)
            print(f'FAIL {check.scenario}: {check.what} = {actual}, expected {expected} ({check.source})', flush=True)
    if hover.stopped:
        print(f'hover part stopped: {hover.stopped}')
    if stopped:
        print(f'stopped early: {stopped}')
    if not failed and not stopped and passed == total:
        print(f'GATE PASS {passed}/{total}')
        return 0
    scenarios = sorted(set(failed), key=failed.index)
    print(f'GATE FAIL {passed}/{total}, {len(failed)} failed: {", ".join(scenarios) or "safety"}'
          + (' (stopped after a safety failure)' if stopped else ''))
    return 1


if __name__ == '__main__':
    sys.exit(main())
