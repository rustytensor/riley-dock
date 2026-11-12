# SPDX-License-Identifier: GPL-3.0-or-later
"""Milestone 2 gate: the dock window on DP-2, measured from Hyprland and compared with fixed expected values.

This ports the Rust dock's four capture gates (riley dock/gates/surface.py, reflections_capture.py,
launcher_capture.py, autohide_capture.py) so that they run against any dock binary with the same command line
(--output NAME, --no-swell, --control-socket PATH, --show), the same layer namespace and the same config, pins and
style files. The Qt dock decides PASS or FAIL; an optional Rust dock runs first as a reference only.

Safety, for a live session:
- Only read-only Hyprland queries (layers, monitors, cursorpos). No dispatch, no synthetic events. The pointer moves
  of the Rust gates exist only behind --hover, which the gate never passes by default.
- DP-2 only, one test dock at a time, and only when DP-2 has no dock layer. Each test dock gets its own HOME and XDG
  config, cache and state folders under a temporary folder (XDG_DATA_HOME stays the real one, read only, for icons and
  desktop entries) and a private control socket that must differ from the session's socket.
- Each test dock is stopped by its own process ID. Afterwards DP-2's reserved area must be what it was before, and the
  session's own docks must be unchanged; if not, the gate stops at once.

Output: one line per check, "PASS <scenario>: <what> = <value>" or "FAIL <scenario>: <what> = <actual>, expected
<expected> (<source>)", "REF ..." lines for the reference dock, then "GATE PASS n/n" or "GATE FAIL passed/n ...".
"""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = 'DP-2'
NAMESPACE = 'riley-dock'
# The only Hyprland requests this gate makes by default; all three are read-only.
QUERIES = frozenset({'layers', 'monitors', 'cursorpos'})
HOVER = False

# ---------------------------------------------------------------------------------------------------------------
# Fixed expected values. Sources: the Rust dock's capture gates and code in the riley repository ("riley <path>:<line>"),
# and the Rust dock's recorded DP-2 captures. None of them is derived from the output of the dock under test.
# ---------------------------------------------------------------------------------------------------------------
APPEAR_SECONDS = 3.0        # riley dock/gates/surface.py:39, reflections_capture.py:99-102, launcher_capture.py:53-56
TOP_LEVEL = '2'             # riley dock/gates/surface.py:51 (layer-shell TOP)
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
REST_SIZE = (921, 144)      # recorded Rust DP-2 capture, 2026-11-12 (reflections rest and launcher left/right/center)
REST_ZONE = 86              # recorded Rust DP-2 capture, 2026-11-12 (same captures, reservation 86)
LAUNCHER_OFF_SIZE = (845, 144)  # recorded Rust DP-2 capture, 2026-11-12 (launcher off, reservation 86)
AUTOHIDE_SIZE = (884, 132)  # recorded Rust DP-2 capture, 2026-11-12 (auto-hide, surface fixed in both poses)

SRC_RECORDED = 'recorded Rust DP-2 capture, 2026-11-12'
SRC_ANCHOR = 'riley dock/src/ui.rs:98-102'   # TOP layer, bottom anchor only (centred), bottom margin 0


class SafetyError(Exception):
    """A live-session rule would be broken: the gate stops at once."""


class Missing(Exception):
    pass


# --- Hyprland, read only ----------------------------------------------------------------------------------------

def query(what):
    if what not in QUERIES:
        raise SafetyError(f'hyprctl {what} is not a read-only query this gate may make')
    result = subprocess.run(['hyprctl', what, '-j'], capture_output=True, text=True, check=True, timeout=2)
    return json.loads(result.stdout)


def dock_layers():
    return [(screen, level, row) for screen, entry in query('layers').items()
            for level, rows in entry['levels'].items() for row in rows
            if row.get('namespace') == NAMESPACE]


def identities():
    return {(screen, row['pid'], row['address']) for screen, _, row in dock_layers()}


def monitor():
    for record in query('monitors'):
        if record['name'] == OUTPUT:
            return record
    raise SafetyError(f'{OUTPUT} is not connected')


def logical_size(record):
    width, height = record['width'] / record['scale'], record['height'] / record['scale']
    if record.get('transform', 0) % 2:
        width, height = height, width
    return round(width), round(height)


def pointer():
    position = query('cursorpos')
    return position['x'], position['y']


def move_pointer(x, y):
    # A synthetic pointer move, exactly as riley dock/gates/reflections_capture.py:29-30. Only with --hover.
    if not HOVER:
        raise SafetyError('a pointer move needs --hover')
    reply = subprocess.run(['hyprctl', 'dispatch', f'hl.dsp.cursor.move({{ x = {int(x)}, y = {int(y)} }})'],
                           capture_output=True, text=True, check=True, timeout=2).stdout
    if reply.strip() != 'ok':
        raise SafetyError(f'pointer move refused: {reply.strip()!r}')


def production_socket():
    # riley dock/gates/autohide_capture.py:18-20
    runtime = os.environ.get('XDG_RUNTIME_DIR', '')
    instance = os.environ.get('HYPRLAND_INSTANCE_SIGNATURE', '')
    return Path(runtime) / 'hypr' / instance / 'riley-dock.sock'


# --- Scenarios ----------------------------------------------------------------------------------------------------
# Each is one test dock, with the isolated config of the Rust gate it ports. "pins": None = no pins file,
# "copy" = copy the session's pins file if there is one, "text" = always write it (empty if there is none).

def scenario(name, gate, config, pins, style, args, kind, size=None, zone=None):
    return dict(name=name, gate=gate, config=config, pins=pins, style=style, args=args, kind=kind,
                size=size, zone=zone)


SCENARIOS = [
    # riley dock/gates/surface.py:25-35: empty config, cache and state; --no-swell.
    scenario('screens', 'surface.py', None, None, False, ['--no-swell'], 'screens'),
    # riley dock/gates/reflections_capture.py:81-85,95
    scenario('rest', 'reflections_capture.py', 'ICON_SIZE=44\nREFLECTIONS=1\n', 'copy', True, [], 'rest',
             REST_SIZE, REST_ZONE),
    # The same with reflections off; checked against 'rest' by the Rust layout rule (riley dock/src/autohide.rs:34-45).
    scenario('reflections-off', 'reflections_capture.py', 'ICON_SIZE=44\nREFLECTIONS=0\n', 'copy', True, [],
             'reflections-off'),
] + [
    # riley dock/gates/launcher_capture.py:32-41,49
    scenario(f'launcher-{mode}', 'launcher_capture.py', f'ICON_SIZE=44\nREFLECTIONS=1\nLAUNCHER={mode}\n', 'text',
             True, [], 'rest', LAUNCHER_OFF_SIZE if mode == 'off' else REST_SIZE, REST_ZONE)
    for mode in ('left', 'right', 'center', 'off')
] + [
    # riley dock/gates/autohide_capture.py:36-41,51: one dock, two scenarios (hidden, then shown after --show).
    scenario('autohide', 'autohide_capture.py', 'ICON_SIZE=44\nREFLECTIONS=1\nAUTOHIDE=1\n', 'copy', True,
             ['--no-swell'], 'autohide', AUTOHIDE_SIZE, HIDDEN_ZONE),
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


def capture(pid, path, scale):
    """riley dock/gates/reflections_capture.py:39-45: the whole surface at DP-2's scale."""
    row = owned_row(pid)
    geometry = f'{row["x"]},{row["y"]} {row["w"]}x{row["h"]}'
    try:
        subprocess.run(['grim', '-s', str(scale), '-g', geometry, str(path)], check=True, capture_output=True,
                       timeout=3)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise Missing(f'screenshot failed: {error}') from error
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


def write_isolated(home, scn):
    config, cache, state = home / 'config', home / 'cache', home / 'state'
    for folder in (config, cache, state):
        folder.mkdir()
    if scn['config'] is not None:
        (config / 'nwg-dock-hyprland').mkdir()
        (config / 'nwg-dock-hyprland/riley.conf').write_text(scn['config'])
    if scn['style']:
        # The Rust gates copy riley dock/style.example.css. The Rust dock built in that file as its fallback
        # (riley dock/src/ui.rs:282-285), so it gets the same style without the copy; the Qt dock gets the JSON file
        # with the same values (docs/files.md, section 3).
        (config / 'riley-dock').mkdir()
        shutil.copyfile(ROOT / 'style.example.json', config / 'riley-dock/style.json')
    real_pins = Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'nwg-dock-pinned'
    if scn['pins'] == 'copy' and real_pins.exists():
        shutil.copyfile(real_pins, cache / 'nwg-dock-pinned')
    elif scn['pins'] == 'text':
        (cache / 'nwg-dock-pinned').write_text(real_pins.read_text() if real_pins.exists() else '')
    env = os.environ.copy()
    env.update(HOME=str(home), XDG_CONFIG_HOME=str(config), XDG_CACHE_HOME=str(cache), XDG_STATE_HOME=str(state),
               XDG_DATA_HOME=os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share')))
    for key, real in (('HOME', Path.home()),
                      ('XDG_CONFIG_HOME', Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config'))),
                      ('XDG_CACHE_HOME', Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache'))),
                      ('XDG_STATE_HOME', Path(os.environ.get('XDG_STATE_HOME', Path.home() / '.local/state')))):
        if Path(env[key]).resolve() == real.resolve() or not Path(env[key]).resolve().is_relative_to(home.resolve()):
            raise SafetyError(f'{key} is not isolated')
    return env


def measure(binary, scn, label, capture_dir):
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
    original_pointer = pointer() if HOVER else None
    child = None
    with tempfile.TemporaryDirectory(prefix='home-', dir=capture_dir) as directory:
        home = Path(directory)
        env = write_isolated(home, scn)
        socket = home / 'control.sock'
        if socket.resolve() == production_socket().resolve() or len(str(socket)) > 100:
            raise SafetyError('the private control socket is not usable')
        m['socket_private'] = True
        diagnostics = DIAGNOSTICS_SURFACE if scn['kind'] == 'screens' else DIAGNOSTICS
        errors_path = home / 'stderr'
        try:
            if HOVER:
                move_pointer(mon['x'] + 2, mon['y'] + 80)   # riley dock/gates/reflections_capture.py:92-93
            with errors_path.open('w+') as errors:
                argv = [str(binary), '--output', OUTPUT, *scn['args'], '--control-socket', str(socket)]
                started = time.monotonic()
                child = subprocess.Popen(argv, env=env, stdout=errors, stderr=errors, stdin=subprocess.DEVNULL,
                                         cwd=home)
                m['pid'] = child.pid
                try:
                    run_dock(child, scn, m, started, binary, env, socket, capture_dir, label)
                except Missing as error:
                    m['why'] = str(error)
                finally:
                    stop(child)
                    errors.flush()
                    errors.seek(0)
                    text = errors.read()
                    m['diagnostics'] = [word for word in diagnostics if word in text]
                    if text.strip():
                        (capture_dir / f'{scn["name"]}-{label}.stderr').write_text(text[-20000:])
        finally:
            if HOVER and original_pointer:
                move_pointer(*original_pointer)
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


def run_dock(child, scn, m, started, binary, env, socket, capture_dir, label):
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
    row, png, outside = capture(pid, path, m['scale'])
    m.update(capture_row=row, png=png, pointer_outside=outside, png_path=str(path))
    if scn['kind'] == 'rest' and scn['name'] == 'rest' and HOVER:
        # riley dock/gates/reflections_capture.py:109-121: one entry into the visible bar, then the swollen surface.
        zone = reserved[3] - m['before'][3]
        move_pointer(row['x'] + row['w'] / 2, row['y'] + row['h'] - 8 - (zone - 8) / 2)
        time.sleep(SWELL_SECONDS)
        if child.poll() is not None:
            raise Missing('dock exited during hover')
        grown, m['swollen_png'], _ = capture(pid, capture_dir / f'swollen-{label}.png', m['scale'])
        m['swollen_size'] = (grown['w'], grown['h'])
        m['swollen_reserved'] = list(monitor()['reserved'])
    if scn['kind'] == 'autohide':
        show(child, m, binary, env, socket, capture_dir, label)


def show(child, m, binary, env, socket, capture_dir, label):
    """riley dock/gates/autohide_capture.py:65-95, without the pointer move unless --hover."""
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
    if HOVER:
        moved = m['moved']
        move_pointer(moved['x'] + moved['w'] / 2, moved['y'] + moved['h'] - 14)   # autohide_capture.py:87-89
        time.sleep(SHOWN_HOLD_SECONDS)
        m['alive'] = child.poll() is None
    # Without the pointer to hold it open the dock hides 700 ms after the reveal, so the shown pose is taken at once.
    _, m['shown_png'], m['shown_pointer_outside'] = capture(child.pid, capture_dir / f'autohide-shown-{label}.png',
                                                            m['scale'])
    m['shown_reserved'] = list(monitor()['reserved'])
    if not HOVER:
        time.sleep(SHOWN_HOLD_SECONDS)
        m['alive'] = child.poll() is None


# --- Checks -------------------------------------------------------------------------------------------------------

def need(m, key):
    if m.get(key) is None:
        raise Missing(m.get('why') or f'{key} not measured')
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


def checks_for(scn):
    """The checks of one scenario, as (scenario, what, function, expected, source).

    A function takes the measurements of this scenario and those of the same dock's other scenarios, and returns
    (ok, actual) or (ok, actual, expected) when the expected text depends on the session (DP-2's reserved area before
    the run, for example). A value that was not measured makes the check fail."""
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
    out = []

    def add(scenario_name, what, fn, expected, source):
        out.append((scenario_name, what, fn, expected, source))

    first = f'{name}-hidden' if kind == 'autohide' else name
    if kind == 'autohide':
        add(first, 'private control socket', lambda m, _: (need(m, 'socket_private'), yes(m['socket_private'])),
            'yes, not the session\'s socket', f'{ah}:41-42')
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
    if scn['size']:
        add(first, 'surface size',
            lambda m, _, s=scn['size']: ((need(m, 'settled')['w'], m['settled']['h']) == s,
                                         size_text((m['settled']['w'], m['settled']['h']))),
            size_text(scn['size']), SRC_RECORDED)

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
            rest = ctx.get('rest') or {}
            r, o = need(rest, 'settled'), need(m, 'settled')
            dz = rest['reserved'][3] - m['reserved'][3]
            dh = r['h'] - o['h']
            return dz == dh and dz > 0, f'surface {dh} lower, reservation {dz} lower than rest'
        add(first, 'reflection lane vs rest', lane, 'surface and reservation lower by the same amount, above 0',
            'riley dock/src/autohide.rs:36-44, dock/src/motion.rs:154-161')

        def width(m, ctx):
            rest = ctx.get('rest') or {}
            return (need(m, 'settled')['w'] == need(rest, 'settled')['w'], str(m['settled']['w']),
                    f'{rest["settled"]["w"]} (as rest)')
        add(first, 'surface width vs rest', width, 'as rest', 'riley dock/src/autohide.rs:39')
    add(first, 'layout at capture',
        lambda m, _: (need(m, 'capture_row') == need(m, 'settled'), f'{m["capture_row"]}'),
        'unchanged since settled', src['same'])
    add(first, 'pointer outside the dock', lambda m, _: (need(m, 'pointer_outside'), yes(m['pointer_outside'])),
        'yes', f'{rc}:92-93 (there the pointer is moved out; here it is only read)')
    add(first, 'screenshot', lambda m, _: (need(m, 'png'), 'PNG' if m['png'] else 'not a PNG'), 'PNG', src['png'])
    if name == 'rest' and HOVER:
        add(first, 'reservation during swell',
            lambda m, _: (need(m, 'swollen_reserved') == m['reserved'], str(m['swollen_reserved'])),
            'as at rest', f'{rc}:116-120')
        add(first, 'surface during swell',
            lambda m, _: (need(m, 'swollen_size') == (m['settled']['w'], m['settled']['h']),
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
    add(last, 'stderr diagnostics',
        lambda m, _: (need(m, 'diagnostics') == [], ', '.join(m['diagnostics']) or 'none'),
        'none of ' + ', '.join(diagnostics), src['diag'])
    add(last, 'layer gone after stop by PID', lambda m, _: (need(m, 'gone'), yes(m['gone'])),
        f'yes, within {CLEANUP_SECONDS:g} s', src['gone'])
    if kind == 'autohide':
        add(last, 'private control socket removed', lambda m, _: (need(m, 'socket_removed'), yes(m['socket_removed'])),
            'yes', f'{ah}:115')
    add(last, f'{OUTPUT} reserved after cleanup',
        lambda m, _: (need(m, 'restored') == m['before'], str(m['restored']), f'{m["before"]} (as before)'),
        'as before the run', src['restored'])
    add(last, 'session docks unchanged', lambda m, _: (need(m, 'identities_same'), yes(m['identities_same'])),
        'yes', src['ids'])
    return out


def evaluate(check, m, ctx):
    """(ok, actual, expected) for one check on one dock's measurements."""
    _, _, fn, expected, _ = check
    try:
        result = fn(m, ctx)
    except Missing as error:
        return False, f'not measured ({error})', expected
    except (KeyError, TypeError, IndexError) as error:
        return False, f'not measured ({error.__class__.__name__}: {error})', expected
    return result[0], result[1], result[2] if len(result) > 2 else expected


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
    if 'show_rc' in m:
        parts.append(f'show exit {m["show_rc"]} after {m.get("show_seconds")} s, {m.get("sample_count")} samples '
                     f'{m.get("samples")}, shown reserved {m.get("shown_reserved")}')
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
    global HOVER
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--qt-dock', required=True)
    parser.add_argument('--rust-dock')
    parser.add_argument('--capture-dir', required=True)
    parser.add_argument('--hover', action='store_true',
                        help='also run the swell-under-hover captures; these move the pointer with hyprctl dispatch')
    options = parser.parse_args()
    HOVER = options.hover
    capture_dir = Path(options.capture_dir)
    capture_dir.mkdir(parents=True, exist_ok=True)
    print(f'qt dock: {options.qt_dock}')
    print(f'reference dock: {options.rust_dock or "none"}')
    print(f'captures: {capture_dir}')
    if HOVER:
        print('HOVER: this run moves the pointer with hyprctl dispatch (synthetic events)')

    all_checks = [(scn, checks_for(scn)) for scn in SCENARIOS]
    total = sum(len(checks) for _, checks in all_checks)
    measures = {'qt': {}, 'rust': {}}
    stopped = None
    failed, passed = [], 0
    for scn, checks in all_checks:
        docks = ([('rust', options.rust_dock)] if options.rust_dock else []) + [('qt', options.qt_dock)]
        for label, binary in docks:
            if stopped:
                measures[label][scn['name']] = {'why': f'not run: stopped after a safety failure ({stopped})'}
                continue
            try:
                m = measure(binary, scn, label, capture_dir)
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
                print(f'REF {scn["name"]}: rust measured {summary(m)}', flush=True)
                for check in checks:
                    ok, actual, expected = evaluate(check, m, measures['rust'])
                    if not ok:
                        print(f'REF MISMATCH {check[0]}: {check[1]} = {actual}, expected {expected} ({check[4]})',
                              flush=True)
        m = measures['qt'][scn['name']]
        for check in checks:
            ok, actual, expected = evaluate(check, m, measures['qt'])
            if ok:
                passed += 1
                print(f'PASS {check[0]}: {check[1]} = {actual}', flush=True)
            else:
                failed.append(check[0])
                print(f'FAIL {check[0]}: {check[1]} = {actual}, expected {expected} ({check[4]})', flush=True)
        poses = ['hidden', 'shown'] if scn['kind'] == 'autohide' else [None]
        for pose in poses:
            stem = f'{scn["name"]}-{pose}' if pose else scn['name']
            qt_png, rust_png = capture_dir / f'{stem}-qt.png', capture_dir / f'{stem}-rust.png'
            side = capture_dir / f'{stem}-side.png'
            if options.rust_dock and side_by_side(rust_png, qt_png, side):
                print(f'IMAGES {stem}: {side}')
            else:
                shots = [str(p) for p in (rust_png, qt_png) if p.exists()]
                print(f'IMAGES {stem}: {", ".join(shots) or "none"}')
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
