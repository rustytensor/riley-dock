# SPDX-License-Identifier: GPL-3.0-or-later
"""Taskbar and grid-hover gate (Marc, 2026-11-13): open apps that are not pinned show with a running dot of their own
colour, and the app-grid button's dots spread apart on hover, smoothly, inside the button's own square.

Runs the given dock in a nested Hyprland on DP-2 (gates/nested.py) with two test windows of its own, an isolated
config, cache and state, and the nest's own cursor and screenshots. Marc's windows, cursor and dock are not touched.

Expected values, worked out before the run from the dock's documented layout (src/riley-dock/motion.h,
src/core/autohide.cpp dockLayout, src/riley-dock/grid.h) for icon-size 44, reflections off, launcher left, two apps:
  drawn size 44*5/4 = 55 (2 apps: divisor 6), rest width 55*0.8 = 44, gap 8, side padding 10;
  surface width ceil(n*44*1.8 + 8*(n-1) + 20): 3 slots 274, 2 slots 187;
  surface height ceil(44*2.2 + 36) + 8 = 141; icon bottom 133 - 17 = 116; running dots at y 116 + 6 = 122, radius 2;
  3 slots: bar 3*44 + 2*8 + 20 = 168 wide at x (274 - 168)/2 = 53; slots at x 63, 115, 167;
    the launcher 63..107 (centre x 85), its square y 66 + (67 - 44)/2 = 77.5 .. 121.5 (centre y 99.5);
    the pinned app's dot at x 115 + 22 = 137, the other app's at 167 + 22 = 189;
  grid dots: step 44/4 * spacing, radius 44/16 = 2.75; centre spread (outer centres) 2*step:
    rest (1.0) 22.0, hover (grid::HOVER_SPACING 1.4) 30.8; outer edges at rest 85 -+ 13.75, hovered 85 -+ 18.15,
    inside the square 63..107.
  colours (style.example.xml): accent #7aa2f7 for a pinned app's dot, unpinned-dot #c0caf5 for the other.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nested  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PINNED, FREE = 'riley-gate-pinned', 'riley-gate-free'
ACCENT, UNPINNED = (0x7a, 0xa2, 0xf7), (0xc0, 0xca, 0xf5)
COLOUR_TOLERANCE = 12          # per channel, 8-bit
WIDTH_3, WIDTH_2, HEIGHT = 274, 187, 141
DOT_Y = 122
DOT_X = {PINNED: 137, FREE: 189}
DOT_X_AFTER_CLOSE = 119.5       # 2 slots: bar 116 wide at 35.5, the pinned slot at 97.5
GRID_CX, GRID_CY = 85.0, 99.5
SLOT = (63.0, 107.0)
GRID_RADIUS = 44 / 16
SPREAD_REST, SPREAD_HOVER = 22.0, 30.8
SPREAD_TOLERANCE = 0.6
FRAMES_SECONDS = 0.45

CONFIG_XML = '''<?xml version="1.0" encoding="UTF-8"?>
<dock xmlns="urn:riley-dock:config" version="1">
  <icon-size>44</icon-size>
  <reflections>false</reflections>
  <launcher>left</launcher>
  <autohide>false</autohide>
</dock>
'''

NEST_LUA = '''-- the dock gate's nest: one output at scale 1, nothing else
hl.config({ xwayland = { enabled = false }, animations = { enabled = false },
            misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
            ecosystem = { no_update_news = true, no_donation_nag = true },
            cursor = { invisible = true } })   -- pointer events as usual, no cursor in the screenshots
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = 1 })
'''

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok)))
    print(('PASS ' if ok else 'FAIL ') + name + (f': {detail}' if detail else ''), flush=True)


# --- pictures ------------------------------------------------------------------------------------------------------

class Picture:
    def __init__(self, data):
        # binary PPM (grim -t ppm): "P6\n<w> <h>\n255\n" then RGB bytes
        parts, pos = [], 0
        while len(parts) < 4:
            while data[pos:pos + 1].isspace():
                pos += 1
            end = pos
            while not data[end:end + 1].isspace():
                end += 1
            parts.append(data[pos:end])
            pos = end
        pos += 1
        if parts[0] != b'P6' or parts[3] != b'255':
            raise ValueError('not an 8-bit PPM')
        self.w, self.h, self.px = int(parts[1]), int(parts[2]), data[pos:]

    def at(self, x, y):
        i = (y * self.w + x) * 3
        return tuple(self.px[i:i + 3])


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def near(c, want):
    return max(abs(a - b) for a, b in zip(c, want)) <= COLOUR_TOLERANCE


def dot_colour(pic, cx, cy):
    """The brightest pixel within 2 px of (cx, cy): a radius-2 disc always covers one whole pixel there."""
    best = None
    for y in range(int(cy) - 2, int(cy) + 3):
        for x in range(int(cx) - 2, int(cx) + 3):
            if 0 <= x < pic.w and 0 <= y < pic.h:
                c = pic.at(x, y)
                if best is None or lum(c) > lum(best):
                    best = c
    return best


def components(pic, x0, x1, y0, y1, threshold=100):
    """Bright 4-connected pixel groups in the box: list of (pixels [(x, y, weight)]) ."""
    seen, out = set(), []
    for y in range(y0, y1):
        for x in range(x0, x1):
            if (x, y) in seen or lum(pic.at(x, y)) <= threshold:
                continue
            stack, group = [(x, y)], []
            seen.add((x, y))
            while stack:
                px, py = stack.pop()
                group.append((px, py, lum(pic.at(px, py))))
                for nx, ny in ((px + 1, py), (px - 1, py), (px, py + 1), (px, py - 1)):
                    if x0 <= nx < x1 and y0 <= ny < y1 and (nx, ny) not in seen and lum(pic.at(nx, ny)) > threshold:
                        seen.add((nx, ny))
                        stack.append((nx, ny))
            out.append(group)
    return out


def centroid(group):
    total = sum(w for _, _, w in group)
    return sum(x * w for x, _, w in group) / total + 0.5, sum(y * w for _, y, w in group) / total + 0.5


def grid_dots(pic, x0=55, x1=113):
    """The app-grid button's nine dots: (centres, leftmost pixel, rightmost pixel), or None."""
    groups = components(pic, x0, x1, 79, 121)
    if len(groups) != 9:
        return None
    centres = [centroid(g) for g in groups]
    xs = [x for g in groups for x, _, _ in g]
    return centres, min(xs), max(xs)


def leftmost_dots(pic):
    """The nine leftmost bright groups in the grid's rows (the launcher is the first slot), and the bright pixels
    right of them: (dot groups, other xs), or None."""
    groups = sorted(components(pic, 0, pic.w, 79, 121), key=lambda g: min(x for x, _, _ in g))
    if len(groups) < 9:
        return None
    return groups[:9], [x for g in groups[9:] for x, _, _ in g]


def spread(centres):
    xs = [c[0] for c in centres]
    return max(xs) - min(xs)


# --- the run -------------------------------------------------------------------------------------------------------

class Run:
    def __init__(self, args):
        self.args = args
        self.work = Path(args.work)
        self.nest = None
        self.pids = {}
        self.children = {}
        self.output = None

    def env(self, home):
        base = {k: v for k, v in os.environ.items()}
        base.update(HOME=str(home), XDG_CONFIG_HOME=str(home / 'config'), XDG_CACHE_HOME=str(home / 'cache'),
                    XDG_STATE_HOME=str(home / 'state'),
                    XDG_DATA_HOME=os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share')))
        for key in ('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_STATE_HOME'):
            if not Path(base[key]).resolve().is_relative_to(self.work.resolve()):
                raise RuntimeError(f'{key} is not isolated')
        return self.nest.env(base)

    def start(self):
        self.work.mkdir(parents=True)
        cfg = self.work / 'hyprland.lua'
        cfg.write_text(NEST_LUA)
        self.nest = nested.Nest(self.args.hyprland, cfg, self.work / 'nest.log').start()
        # The nest's own window sits on a hidden workspace and gets no frames from the session, so the gate works on
        # a headless output of the nest, which draws on its own clock.
        before = {m['name'] for m in self.nest.query('monitors')}
        self.nest.ctl('output', 'create', 'headless', 'GATE-1')
        mon = self.nest.wait(lambda: next((m for m in self.nest.query('monitors') or [] if m['name'] not in before), None))
        if not mon:
            raise RuntimeError('no headless output in the nest')
        self.output = mon['name']
        print(f'nest: pid {self.nest.pid}, output {mon["name"]} {mon["width"]}x{mon["height"]} at {mon["x"]},{mon["y"]} '
              f'scale {mon["scale"]}', flush=True)
        self.nest.dispatch(f'hl.dsp.cursor.move({{ x = {mon["x"] + 10}, y = {mon["y"] + 10} }})')
        home = self.work / 'home'
        for d in ('config/riley-dock', 'cache', 'state'):
            (home / d).mkdir(parents=True)
        (home / 'config/riley-dock/config.xml').write_text(CONFIG_XML)
        shutil.copyfile(ROOT / 'style.example.xml', home / 'config/riley-dock/style.xml')
        self.pins = home / 'cache/nwg-dock-pinned'
        self.pins.write_text(PINNED + '\n')
        self.home = home
        env = self.env(home)
        for cls in (PINNED, FREE):
            self.window(cls)
        if not self.nest.wait(lambda: {c['class'] for c in self.nest.query('clients') or []} >= {PINNED, FREE}, 15):
            raise RuntimeError('the test windows did not open in the nest')
        self.dock = subprocess.Popen([self.args.qt_dock, '--output', self.output], env=env, stdout=open(self.work / 'dock.out', 'w'),
                                     stderr=subprocess.STDOUT, start_new_session=True)
        self.children['dock'] = self.dock

    def window(self, cls):
        self.children[cls] = subprocess.Popen(
            ['kitty', '--config', 'NONE', '--class', cls, '-o', 'linux_display_server=wayland',
             '-o', 'confirm_os_window_close=0', 'sleep', '600'], env=self.env(self.home), stdout=subprocess.DEVNULL,
            stderr=open(self.work / f'{cls}.log', 'a'), start_new_session=True)

    def close(self, key):
        child = self.children.pop(key)
        child.terminate()
        try:
            child.wait(5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(5)

    def layer(self):
        for name, mon in (self.nest.query('layers') or {}).items():
            if name != self.output:
                continue
            for rows in mon['levels'].values():
                for row in rows:
                    if row['namespace'] == 'riley-dock' and row['pid'] == self.dock.pid:
                        return row
        return None

    def settled(self, width, timeout=8):
        def ok():
            row = self.layer()
            return row if row and row['w'] == width and row['h'] == HEIGHT else None
        return self.nest.wait(ok, timeout)

    def shot(self, row=None):
        row = row or self.layer()
        out = subprocess.run(['grim', '-t', 'ppm', '-s', '1', '-g', f'{row["x"]},{row["y"]} {row["w"]}x{row["h"]}',
                              '-'], env=self.nest.env(os.environ), capture_output=True, timeout=5)
        if out.returncode != 0:
            raise RuntimeError(f'grim: {out.stderr.decode(errors="replace").strip()}')
        return Picture(out.stdout)

    def move(self, row, x, y):
        """The nest's cursor to (x, y) on the dock. A warp inside one surface sends that surface no motion, so the
        cursor first leaves the dock (100 px above it) and then enters it at (x, y)."""
        self.nest.dispatch(f'hl.dsp.cursor.move({{ x = {row["x"] + x}, y = {row["y"] - 100} }})')
        self.nest.dispatch(f'hl.dsp.cursor.move({{ x = {row["x"] + x}, y = {row["y"] + y} }})')

    def stop(self):
        for key in list(self.children):
            self.close(key)
        if self.nest:
            self.nest.stop()


def taskbar(run):
    row = run.settled(WIDTH_3)
    got = run.layer()
    check('1.0 dock with launcher, pinned app and open unpinned app: 3 slots',
          row is not None, f'surface {got and (got["w"], got["h"])}, expected {WIDTH_3}x{HEIGHT}')
    if not row:
        return
    time.sleep(0.5)
    pic = run.shot(row)
    pinned, free = dot_colour(pic, DOT_X[PINNED], DOT_Y), dot_colour(pic, DOT_X[FREE], DOT_Y)
    check('1.1 pinned app: running dot in accent', near(pinned, ACCENT) and not near(pinned, UNPINNED),
          f'{pinned} at x {DOT_X[PINNED]}, expected {ACCENT}')
    check('1.2 unpinned app: running dot in unpinned-dot', near(free, UNPINNED) and not near(free, ACCENT),
          f'{free} at x {DOT_X[FREE]}, expected {UNPINNED}')
    # Pinning it (the pins file, which the menu's Pin also writes) turns its dot to the accent; unpinning back.
    run.pins.write_text(f'{PINNED}\n{FREE}\n')
    turned = run.nest.wait(lambda: near(dot_colour(run.shot(row), DOT_X[FREE], DOT_Y), ACCENT), 5, 0.2)
    check('1.3 pinned from the pins file: its dot turns to accent', turned,
          f'{dot_colour(run.shot(row), DOT_X[FREE], DOT_Y)}')
    run.pins.write_text(PINNED + '\n')
    back = run.nest.wait(lambda: near(dot_colour(run.shot(row), DOT_X[FREE], DOT_Y), UNPINNED), 5, 0.2)
    check('1.4 unpinned again: its dot back to unpinned-dot', back, f'{dot_colour(run.shot(row), DOT_X[FREE], DOT_Y)}')
    # Its last window closes: the icon leaves the dock.
    run.close(FREE)
    gone = run.settled(WIDTH_2)
    got = run.layer()
    check('1.5 unpinned app closed: its icon leaves (2 slots)', gone is not None,
          f'surface {got and (got["w"], got["h"])}, expected {WIDTH_2}x{HEIGHT}')
    if gone:
        time.sleep(0.3)
        c = dot_colour(run.shot(gone), DOT_X_AFTER_CLOSE, DOT_Y)
        check('1.6 the pinned app keeps its accent dot', near(c, ACCENT), f'{c} at x {DOT_X_AFTER_CLOSE}')


def grid_hover(run):
    # A fresh row with the free app again, so the grid button has a neighbour on the right that can swell.
    run.window(FREE)
    row = run.settled(WIDTH_3, 15)
    if not row:
        check('2.0 dock back at 3 slots for the hover', False, f'surface {run.layer()}')
        return
    time.sleep(0.5)
    rest = grid_dots(run.shot(row))
    check('2.1 at rest: nine dots, spread 22.0', rest and abs(spread(rest[0]) - SPREAD_REST) <= SPREAD_TOLERANCE,
          f'spread {rest and round(spread(rest[0]), 2)}')
    frames = []
    run.move(row, GRID_CX, GRID_CY)
    start = time.monotonic()
    while time.monotonic() - start < FRAMES_SECONDS:
        frames.append((round(time.monotonic() - start, 3), grid_dots(run.shot(row))))
    time.sleep(0.3)
    hover = grid_dots(run.shot(row))
    ok = hover and abs(spread(hover[0]) - SPREAD_HOVER) <= SPREAD_TOLERANCE
    check('2.2 hovered: spread 30.8 (wider than at rest)', ok and spread(hover[0]) > spread(rest[0]) if rest else False,
          f'spread {hover and round(spread(hover[0]), 2)}')
    if hover:
        cs, left, right = hover
        cx = [c[0] for c in cs]
        check('2.3 hovered: every dot inside the button 63..107',
              left >= SLOT[0] and right < SLOT[1] and min(cx) - GRID_RADIUS >= SLOT[0] - 0.5
              and max(cx) + GRID_RADIUS <= SLOT[1] + 0.5,
              f'pixels {left}..{right}, edges {min(cx) - GRID_RADIUS:.2f}..{max(cx) + GRID_RADIUS:.2f}, '
              f'expected {GRID_CX - 18.15:.2f}..{GRID_CX + 18.15:.2f}')
        # The neighbour's icon square is y 72..116 (icon bottom 116, size 44), the same brightness threshold.
        neighbour = [x for g in components(run.shot(row), int(right) + 1, 160, 72, 116) for x, _, _ in g]
        check('2.4 hovered: a gap to the neighbour icon', neighbour and min(neighbour) > right,
              f'dots end at x {right}, neighbour starts at {neighbour and min(neighbour)}')
    seen = [round(spread(f[0]), 2) for _, f in frames if f]
    mid = [s for s in seen if SPREAD_REST + SPREAD_TOLERANCE < s < SPREAD_HOVER - SPREAD_TOLERANCE]
    check('2.5 on the way: a frame between rest and hover', mid, f'{len(seen)} frames, spreads {seen}')
    steady = all(b >= a - 0.3 for a, b in zip(seen, seen[1:]))
    check('2.6 on the way: the spread only grows (no jump back)', seen and steady, f'{seen}')
    # Off the button onto the neighbour: it swells while the dots settle back; never touching.
    run.move(row, 137, GRID_CY)
    start, touching, frames = time.monotonic(), [], 0
    while time.monotonic() - start < 0.8:
        pic = run.shot(row)
        found = leftmost_dots(pic)
        if not found or not found[1]:
            continue
        frames += 1
        dots, rest_x = found
        right = max(x for g in dots for x, _, _ in g)
        if min(rest_x) <= right:
            touching.append((round(time.monotonic() - start, 3), right, min(rest_x)))
    check('2.7 neighbour swelling: never touches the grid dots', frames > 0 and not touching,
          f'{frames} frames, touching {touching[:3]}')
    time.sleep(0.6)
    found = leftmost_dots(run.shot(row))
    back = found and spread([centroid(g) for g in found[0]])
    check('2.8 back at rest after leaving', back and abs(back - SPREAD_REST) <= SPREAD_TOLERANCE,
          f'spread {back and round(back, 2)}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--qt-dock', required=True)
    ap.add_argument('--hyprland', default='/usr/bin/Hyprland')
    ap.add_argument('--work', required=True)
    args = ap.parse_args()
    run = Run(args)
    try:
        run.start()
        taskbar(run)
        grid_hover(run)
        out = (run.work / 'dock.out').read_text(errors='replace')
        bad = [line for line in out.splitlines() if 'error' in line.lower() or 'warning' in line.lower()]
        check('3.0 dock wrote no error or warning', not bad, '; '.join(bad[:3]))
    except Exception as error:   # the gate says FAIL, never a traceback alone
        check('setup', False, f'{type(error).__name__}: {error}')
    finally:
        run.stop()
        left = [c.pid for c in run.children.values() if c.poll() is None]
        if run.nest and run.nest.pid and Path(f'/proc/{run.nest.pid}').exists():
            left.append(run.nest.pid)
        check('end: nothing of the gate left running', not left, f'{left}')
        shutil.rmtree(run.work / 'home', ignore_errors=True)
    passed = sum(ok for _, ok in results)
    if passed == len(results):
        print(f'GATE PASS {passed}/{len(results)}')
        return 0
    print(f'GATE FAIL {passed}/{len(results)}: ' + ', '.join(n.split()[0] for n, ok in results if not ok))
    return 1


if __name__ == '__main__':
    sys.exit(main())
