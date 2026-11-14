# SPDX-License-Identifier: GPL-3.0-or-later
"""A nested Hyprland for the dock's live gates: one window on an empty DP-2 workspace of the session, with its own
config, its own windows, its own cursor and its own screenshots.

Nothing in it reaches the session: it starts with no systemd or D-Bus environment update (HYPRLAND_NO_SD_VARS,
HYPRLAND_NO_SD_TARGET), without Xwayland, and every program of the gate connects to the nest's own Wayland socket
and Hyprland instance. The session is asked for one thing only: to start the nest silently on the workspace (DP-2 is
not switched, focus stays where it is). Everything is ended by process ID. (Same harness as riley tests/popups.)
"""
import json
import os
import signal
import subprocess
import time
from pathlib import Path

RUNTIME = Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}'))
HYPR = RUNTIME / 'hypr'
WORKSPACE = int(os.environ.get("NEST_WORKSPACE", "97"))   # an empty workspace, opened on DP-2 only for the test


def parent(*args):
    return subprocess.run(['hyprctl', *args], capture_output=True, text=True, timeout=10).stdout


def lua_str(s):
    return '[==[' + s + ']==]'


class Nest:
    def __init__(self, hyprland, config, log):
        self.hyprland, self.config, self.log = str(hyprland), str(config), Path(log)
        self.pid = self.sig = self.wayland = None

    def start(self, timeout=20):
        # An empty workspace 77 may still exist after an earlier nest closed; only windows there count.
        if any(w.get('id') == WORKSPACE and w['windows'] > 0 for w in json.loads(parent('workspaces', '-j'))):
            raise RuntimeError(f'workspace {WORKSPACE} is in use in the session')
        before = {p.name for p in HYPR.iterdir()} if HYPR.exists() else set()
        cmd = (f'env -u HYPRLAND_INSTANCE_SIGNATURE HYPRLAND_NO_SD_VARS=1 HYPRLAND_NO_SD_TARGET=1 '
               f'{self.hyprland} --config {self.config} >{self.log} 2>&1')
        parent('dispatch', f'hl.dsp.exec_cmd({lua_str(cmd)}, {{ monitor = "DP-2", workspace = "{WORKSPACE} silent" }})')
        end = time.time() + timeout
        while time.time() < end:
            for d in HYPR.iterdir():
                lock = d / 'hyprland.lock'
                if d.name in before or not lock.exists() or not (d / '.socket.sock').exists():
                    continue
                lines = lock.read_text().split()
                pid = int(lines[0])
                if os.path.realpath(f'/proc/{pid}/exe') == os.path.realpath(self.hyprland):
                    self.pid, self.sig = pid, d.name
                    if len(lines) > 1:
                        self.wayland = lines[1]
            if self.sig and self.wayland and self.ctl('monitors', '-j').strip().startswith('['):
                return self
            time.sleep(0.2)
        raise RuntimeError(f'the nested Hyprland did not start; log {self.log}')

    def env(self, base):
        """BASE with the nest's Wayland socket and Hyprland instance, never the session's."""
        env = dict(base)
        env.update(WAYLAND_DISPLAY=self.wayland, HYPRLAND_INSTANCE_SIGNATURE=self.sig, QT_QPA_PLATFORM='wayland')
        for key in ('DISPLAY', 'HYPRLAND_CMD', 'NIRI_SOCKET', 'SWAYSOCK'):
            env.pop(key, None)
        if env['WAYLAND_DISPLAY'] == os.environ.get('WAYLAND_DISPLAY'):
            raise RuntimeError('the nest has the session\'s Wayland socket')
        return env

    def ctl(self, *args):
        return subprocess.run(['hyprctl', '-i', self.sig, *args], capture_output=True, text=True, timeout=10).stdout

    def dispatch(self, lua):
        return self.ctl('dispatch', lua)

    def query(self, what):
        try:
            return json.loads(self.ctl(what, '-j'))
        except ValueError:
            return None

    def wait(self, test, timeout=10, step=0.1):
        end = time.time() + timeout
        while time.time() < end:
            got = test()
            if got:
                return got
            time.sleep(step)
        return None

    def stop(self):
        if self.pid:
            end_pids([self.pid])


def end_pids(pids, wait=5):
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    end = time.time() + wait
    while time.time() < end and any(Path(f'/proc/{p}').exists() for p in pids):
        time.sleep(0.1)
    for pid in pids:
        if Path(f'/proc/{pid}').exists():
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
