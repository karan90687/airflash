#!/usr/bin/env python3
"""airflash laptop tool: flash and monitor an ESP over Wi-Fi (airflash updater + airflash component).

  python airflash.py flash    <project> [--no-build] [--force]   build, check, hand over, upload
  python airflash.py monitor  <project>                          idf.py monitor over Wi-Fi (crash decoding)
  python airflash.py logs                                        plain log stream (no ESP-IDF needed)
  python airflash.py coredump <project>                          download + decode the last crash dump
  python airflash.py info                                        what's running on the board
  python airflash.py run                                         updater: boot the installed app

Board address: --host <ip> or env AIRFLASH_HOST (default airflash.local). Token: --token or AIRFLASH_TOKEN.
Standard library only: it runs on the Python that ESP-IDF installs.
"""
import argparse
import hashlib
import http.client
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

UPDATER_PORT = 80
CHIP_NAMES = {0: 'esp32', 2: 'esp32s2', 5: 'esp32c3', 9: 'esp32s3', 12: 'esp32c2', 13: 'esp32c6',
              16: 'esp32h2', 18: 'esp32p4', 20: 'esp32c61', 23: 'esp32c5'}


class Fail(Exception):
    """A problem to report to the user (no traceback)."""


def kb(n):
    return f'{n / 1024:.0f} KB' if n < 1024 * 1024 else f'{n / 1024 / 1024:.1f} MB'


def rev(r):
    return f'v{r // 100}.{r % 100}'


def chip_name(chip_id):
    return CHIP_NAMES.get(chip_id, f'chip id {chip_id}')


# ---------- reading ESP-IDF build output ----------

def read_app_image(path):
    """Fields of esp_image_header_t (offset 0) and esp_app_desc_t (offset 0x20) the board cares about."""
    with open(path, 'rb') as f:
        head = f.read(0xB0)
    if len(head) < 0xB0 or head[0] != 0xE9:
        raise Fail(f'{path} is not an ESP app image')
    chip_id, = struct.unpack_from('<H', head, 12)
    min_rev, max_rev = struct.unpack_from('<HH', head, 15)
    has_desc = struct.unpack_from('<I', head, 0x20)[0] == 0xABCD5432

    def text(off, n):
        return head[off:off + n].split(b'\0')[0].decode(errors='replace') if has_desc else '?'

    return {'chip_id': chip_id, 'min_rev': min_rev, 'max_rev': max_rev, 'size': os.path.getsize(path),
            'version': text(0x30, 32), 'project': text(0x50, 32), 'idf': text(0x90, 32)}


def read_partition_table(path):
    """32-byte entries: magic aa 50, type, subtype, offset, size, 16-byte label (then an MD5 entry, then 0xFF)."""
    with open(path, 'rb') as f:
        data = f.read()
    parts = []
    for off in range(0, len(data) - 31, 32):
        magic, ptype, subtype, offset, size, label = struct.unpack_from('<HBBII16s', data, off)
        if magic != 0x50AA:  # bytes aa 50, little-endian
            break
        parts.append({'label': label.split(b'\0')[0].decode(), 'type': ptype, 'subtype': subtype,
                      'offset': offset, 'size': size})
    return parts


def read_project(project_dir):
    build = os.path.join(project_dir, 'build')
    try:
        with open(os.path.join(build, 'project_description.json')) as f:
            desc = json.load(f)
        with open(os.path.join(build, 'flasher_args.json')) as f:
            flasher = json.load(f)
    except FileNotFoundError:
        raise Fail(f'no build found in {project_dir} (build it first, or drop --no-build)')
    pt = flasher.get('partition-table', {}).get('file')
    try:
        with open(os.path.join(build, 'config', 'sdkconfig.json')) as f:
            coredump = bool(json.load(f).get('ESP_COREDUMP_ENABLE_TO_FLASH'))
    except (OSError, ValueError):
        coredump = None  # unknown
    return {
        'app_bin': os.path.join(desc['build_dir'], desc['app_bin']),
        'elf': os.path.join(desc['build_dir'], desc['app_elf']),
        'partition_table': os.path.join(desc['build_dir'], pt) if pt else None,
        'has_airflash': any(c == 'airflash' or c.endswith('__airflash') for c in desc.get('build_components', [])),
        'coredump': coredump,
    }


def check_compatibility(img, proj, board):
    """Everything that would make the app fail on this board. Returns (blocking problems, warnings)."""
    problems, warnings = [], []
    chip, slot = board['chip'], board['app']
    if img['chip_id'] != chip['image_chip_id']:
        problems.append(f"app is built for {chip_name(img['chip_id'])}, the board is {chip['target']}")
    if img['size'] > slot['slot_size']:
        problems.append(f"app is {kb(img['size'])}, the board's app slot holds {kb(slot['slot_size'])}")
    if not img['min_rev'] <= chip['revision'] <= img['max_rev']:
        problems.append(f"app supports chip {rev(img['min_rev'])}..{rev(img['max_rev'])}, "
                        f"the board is {rev(chip['revision'])}")
    if proj['partition_table'] and os.path.exists(proj['partition_table']):
        on_board = {p['label']: p for p in board['partitions']}
        for p in read_partition_table(proj['partition_table']):
            if p['type'] != 1:  # app partitions: the board's own layout decides where the app goes
                continue
            b = on_board.get(p['label'])
            if b is None:
                problems.append(f"app uses data partition '{p['label']}' ({kb(p['size'])}), the board has none")
            elif b['size'] < p['size']:
                warnings.append(f"partition '{p['label']}': app expects {kb(p['size'])}, board has {kb(b['size'])}")
            elif b['subtype'] != p['subtype']:
                warnings.append(f"partition '{p['label']}': different type (app {p['subtype']:#x}, board {b['subtype']:#x})")
    if not proj['has_airflash']:
        warnings.append("this app doesn't include the airflash component: no wireless logs, "
                        "and the next flash needs the recovery pin")
    elif proj.get('coredump') is False:
        warnings.append("crash dumps are off, so a crash won't show where it happened: "
                        "menuconfig → Core dump → Data destination → Flash (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y)")
    return problems, warnings


def coredump_elf_sha(raw):
    """The build ID inside an ESP core dump: the ESP_CORE_DUMP_INFO note = name, u32 version, ELF SHA-256 (hex)."""
    i = raw.find(b'ESP_CORE_DUMP_INFO\0')
    return raw[i + 24:i + 24 + 65].split(b'\0')[0].decode(errors='replace') if i >= 0 else ''


# ---------- log stream with resume ----------

def resume_note(old_id, old_off, new_id, start):
    """What to tell the user after (re)connecting, or None."""
    if old_id == 0:
        return None  # first connection
    if new_id != old_id:
        return '\n[airflash: the board\'s log buffer started over (power-off, new app or update)]\n'
    if start > old_off:
        return f'\n[airflash: {start - old_off} bytes of logs were overwritten while disconnected]\n'
    return None


def board_stream(host, port, quiet=5.0, retry=1.0):
    """Yield log bytes forever. The board's buffer survives reboots and keeps its offsets, so after a reconnect
    (network glitch, crash, reboot) we ask for exactly where we stopped: no duplicates, nothing missed.
    Silence for `quiet` seconds = reconnect (a crashed board never closes its old connection)."""
    log_id, offset, unreachable = 0, 0, False
    while True:
        try:
            s = socket.create_connection((host, port), timeout=5)
        except OSError:
            if not unreachable:
                yield f'\n[airflash: board unreachable at {host}:{port}, retrying...]\n'.encode()
                unreachable = True
            time.sleep(retry)
            continue
        unreachable = False
        with s:
            try:
                s.settimeout(quiet)
                s.sendall(f'airflash {log_id:08x} {offset:x}\n'.encode())
                buf = b''
                while b'\n' not in buf and len(buf) < 80:
                    chunk = s.recv(80)
                    if not chunk:
                        raise OSError('closed')
                    buf += chunk
                line, _, rest = buf.partition(b'\n')
                parts = line.split()
                framed = len(parts) == 3 and parts[0] == b'airflash'
                if framed:
                    new_id, start = int(parts[1], 16), int(parts[2], 16)
                    note = resume_note(log_id, offset, new_id, start)
                    if note:
                        yield note.encode()
                    log_id, offset = new_id, start
                else:
                    rest = buf  # component older than 0.3: no hello, it's all log data
                while True:
                    if rest:
                        if framed:
                            offset += len(rest)
                        yield rest
                    rest = s.recv(4096)
                    if not rest:
                        break
            except (OSError, ValueError):
                pass  # silence, reset or garbage: reconnect and resume
        time.sleep(retry)


# ---------- talking to the board ----------

class Board:
    def __init__(self, host, token, app_port, log_port):
        self.host, self.token, self.app_port, self.log_port = host, token, app_port, log_port

    def _headers(self):
        return {'X-Token': self.token} if self.token else {}

    def request(self, method, port, path, timeout=3):
        conn = http.client.HTTPConnection(self.host, port, timeout=timeout)
        try:
            conn.request(method, path, headers=self._headers())
            r = conn.getresponse()
            return r.status, r.read()
        finally:
            conn.close()

    def _json(self, port, path):
        try:
            status, body = self.request('GET', port, path, timeout=2)
        except (OSError, http.client.HTTPException):
            return None
        if status == 401:
            raise Fail('the board wants a token: --token or AIRFLASH_TOKEN')
        try:
            return json.loads(body) if status == 200 else None
        except ValueError:
            return None  # e.g. the app's own web server answered on port 80

    def app_info(self):
        j = self._json(self.app_port, '/info')
        return j if j and j.get('role') == 'app' else None

    def updater_info(self):
        j = self._json(UPDATER_PORT, '/info')
        return j if j and 'updater' in j else None

    def who(self, wait):
        """App and updater share the IP, so ask both. Keep asking while the board (re)joins Wi-Fi."""
        deadline = time.time() + wait
        while True:
            info = self.app_info()
            if info:
                return 'app', info
            info = self.updater_info()
            if info:
                return 'updater', info
            if time.time() > deadline:
                return None, None
            time.sleep(1)

    def wait_for(self, role, wait):
        deadline = time.time() + wait
        while time.time() < deadline:
            info = self.app_info() if role == 'app' else self.updater_info()
            if info:
                return info
            time.sleep(1)
        return None

    def post(self, port, path):
        status, body = self.request('POST', port, path, timeout=10)
        if status != 200:
            raise Fail(f'{path} failed: {status} {body.decode(errors="replace").strip()}')

    def upload(self, path):
        size = os.path.getsize(path)
        conn = http.client.HTTPConnection(self.host, UPDATER_PORT, timeout=60)
        conn.putrequest('POST', '/update')
        conn.putheader('Content-Type', 'application/octet-stream')
        conn.putheader('Content-Length', str(size))
        for k, v in self._headers().items():
            conn.putheader(k, v)
        conn.endheaders()
        start, sent = time.time(), 0
        with open(path, 'rb') as f:
            while True:
                chunk = f.read(16 * 1024)
                if not chunk:
                    break
                conn.send(chunk)
                sent += len(chunk)
                speed = sent / max(time.time() - start, 0.001) / 1024
                line = f'  uploading {sent * 100 // size:3d}%  {kb(sent)} / {kb(size)}  ({speed:.0f} KB/s)'
                print(f'\r{line:<60}', end='', flush=True)  # pad: \r doesn't erase a longer previous line
        print('\n  verifying on the board (checksum, SHA-256, chip)...')
        r = conn.getresponse()
        body = r.read().decode(errors='replace').strip()
        conn.close()
        if r.status != 200:
            raise Fail(f'the updater refused the image: {body}')


def board_from(a):
    return Board(a.host, a.token, a.app_port, a.log_port)


def not_found(host):
    return Fail(f'board not reachable at {host}: off, still joining Wi-Fi, on another network, '
                f'or the name does not resolve (try --host <ip>)')


# ---------- commands ----------

def cmd_flash(a):
    if not a.no_build:
        idf_py(['build'], a.project)
    proj = read_project(a.project)
    img = read_app_image(proj['app_bin'])
    print(f"app: {img['project']} {img['version']} (IDF {img['idf']}, {chip_name(img['chip_id'])}), {kb(img['size'])}")

    board = board_from(a)
    print(f'looking for the board at {a.host}...')
    role, info = board.who(wait=40)
    if role is None:
        raise not_found(a.host)

    handed_over = False
    if role == 'app':
        print(f"{info['project']} {info['version']} is running → asking it to reboot into the updater")
        board.post(a.app_port, '/reboot-to-updater')
        handed_over = True
        time.sleep(2)
        print('waiting for the updater...')
        info = board.wait_for('updater', 40)
        if info is None:
            raise Fail('the app rebooted but the updater never answered (check the board with `logs`)')

    problems, warnings = check_compatibility(img, proj, info)
    for w in warnings:
        print(f'  warning: {w}')
    if problems and not a.force:
        for p in problems:
            print(f'  error: {p}')
        if handed_over:
            board.post(UPDATER_PORT, '/run')  # don't leave the board without its app
            print('booted the previously installed app again')
        raise Fail('not flashing; fix the problems above or use --force')

    board.upload(proj['app_bin'])
    if not proj['has_airflash']:
        print('done: the new app is booting (it has no airflash component, so it can\'t report back)')
        return
    print('waiting for the new app...')
    app = board.wait_for('app', 60)
    if app:
        print(f"done: {app['project']} {app['version']} is running ({app['state']})")
    else:
        updater = board.updater_info()
        if updater:
            hint = ' It crashed: run `coredump` to see where.' if updater.get('coredump') else ''
            raise Fail('the new app did not confirm itself and the board rolled back to the updater.' + hint)
        print('uploaded; the app has not answered yet (still joining Wi-Fi?) — check with `info` or `logs`')


def cmd_monitor(a):
    # The real idf.py monitor (colors, backtrace decoding) reads a local relay instead of the board itself:
    # the relay keeps one resumable board connection, so the monitor survives crashes and reboots like on USB.
    srv = socket.create_server(('127.0.0.1', 0))
    stream = board_stream(a.host, a.log_port)

    def relay():
        pending = b''
        while True:
            conn, _ = srv.accept()  # the monitor (it reconnects if we ever drop it)
            with conn:
                try:
                    if pending:
                        conn.sendall(pending)
                        pending = b''
                    for chunk in stream:
                        try:
                            conn.sendall(chunk)
                        except OSError:
                            pending = chunk  # keep it for the monitor's next connection
                            break
                except OSError:
                    pass

    threading.Thread(target=relay, daemon=True).start()
    sys.exit(idf_py(['-p', f'socket://127.0.0.1:{srv.getsockname()[1]}', 'monitor'], a.project, check=False))


def cmd_logs(a):
    out = sys.stdout.buffer
    print(f'log stream from {a.host}:{a.log_port} (Ctrl+C to stop)', file=sys.stderr)
    for chunk in board_stream(a.host, a.log_port):
        out.write(chunk)
        out.flush()


def cmd_coredump(a):
    proj = read_project(a.project)
    board = board_from(a)
    role, _ = board.who(wait=10)
    if role is None:
        raise not_found(a.host)
    port = a.app_port if role == 'app' else UPDATER_PORT  # an app that crashed before confirming: the updater has it
    status, raw = board.request('GET', port, '/coredump', timeout=30)
    if status == 404:
        raise Fail('no crash dump on the board (no crash since the last download, crash dumps off, '
                   'or a component/updater older than 0.3)')
    if status != 200:
        raise Fail(f'/coredump failed: {status}')

    path = os.path.join(a.project, 'build', 'airflash_coredump.bin')
    with open(path, 'wb') as f:
        f.write(raw)
    board.request('DELETE', port, '/coredump')  # the board's copy is a mailbox: something there = a NEW crash
    print(f'saved {path} ({kb(len(raw))}) and cleared it on the board')

    # the dump only holds addresses; the exact build's ELF turns them into file:line, so check it's the same build
    dump_sha = coredump_elf_sha(raw)
    with open(proj['elf'], 'rb') as f:
        elf_sha = hashlib.sha256(f.read()).hexdigest()
    if dump_sha and not elf_sha.startswith(dump_sha):
        raise Fail(f'the dump is from another build (ELF SHA {dump_sha}..., this build {elf_sha[:len(dump_sha)]}...). '
                   f'Decode it with that build: idf.py coredump-info -c {path}')
    idf_py(['coredump-info', '-c', path], a.project, check=False)


def cmd_info(a):
    role, info = board_from(a).who(wait=5)
    if role is None:
        raise not_found(a.host)
    dump = info.get('coredump')
    dump_line = f'crash dump waiting ({kb(dump)}): run `coredump`' if dump else 'no crash dump'
    if role == 'app':
        print(f"app running: {info['project']} {info['version']} (IDF {info['idf']}), state {info['state']}, "
              f"airflash {info['airflash']}, logs on port {info['log_port']}")
        if 'boot' in info:
            print(f"boot #{info['boot']}, last reset: {info['reset_reason']}, {dump_line}")
        return
    u, c, app = info['updater'], info['chip'], info['app']
    print(f"updater {u['version']} running because: {u['running_because']} (reset: {u['reset_reason']})")
    if dump is not None:
        print(dump_line)
    print(f"chip: {c['target']} {rev(c['revision'])}, flash {kb(c['flash_size'])}, mac {c['mac']}")
    if app['installed']:
        print(f"installed app: {app['project']} {app['version']} (IDF {app['idf']}), state {app['state']}")
    else:
        print('installed app: none')
    print('partitions:')
    for p in info['partitions']:
        print(f"  {p['label']:10} {'app' if p['type'] == 0 else 'data'}  {p['offset']:#010x}  {kb(p['size'])}")


def cmd_run(a):
    board = board_from(a)
    if not board.updater_info():
        raise Fail('the updater is not running (an app is, or the board is unreachable)')
    board.post(UPDATER_PORT, '/run')
    print('booting the installed app')


def idf_py(args, cwd, check=True):
    idf_path = os.environ.get('IDF_PATH')
    if not idf_path:
        raise Fail('needs an ESP-IDF terminal (run export.sh first)')
    # Run idf.py exactly the way a user types it (from PATH): ESP-IDF refuses to build when a project is
    # built by a different Python executable name than the one it was configured with (python vs python3).
    # Fallback for shells where idf.py isn't on PATH: the current Python + the idf.py script.
    exe = shutil.which('idf.py')
    cmd = [exe] if exe else [sys.executable, os.path.join(idf_path, 'tools', 'idf.py')]
    code = subprocess.call(cmd + args, cwd=cwd)
    if check and code:
        raise Fail(f'idf.py {" ".join(args)} failed')
    return code


def main(argv=None):
    common = argparse.ArgumentParser(add_help=False)  # options accepted after every command
    common.add_argument('--host', default=os.environ.get('AIRFLASH_HOST', 'airflash.local'), help='board IP or name')
    common.add_argument('--token', default=os.environ.get('AIRFLASH_TOKEN', ''))
    common.add_argument('--app-port', type=int, default=8080)
    common.add_argument('--log-port', type=int, default=3333)

    p = argparse.ArgumentParser(prog='airflash', description='Flash and monitor an ESP over Wi-Fi.')
    sub = p.add_subparsers(dest='cmd', required=True)
    f = sub.add_parser('flash', parents=[common], help='build, check, hand over and upload')
    f.add_argument('project', nargs='?', default='.')
    f.add_argument('--no-build', action='store_true', help='upload the existing build')
    f.add_argument('--force', action='store_true', help='flash even if the compatibility checks fail')
    m = sub.add_parser('monitor', parents=[common], help='idf.py monitor over Wi-Fi')
    m.add_argument('project', nargs='?', default='.')
    sub.add_parser('logs', parents=[common], help='plain log stream')
    c = sub.add_parser('coredump', parents=[common], help='download and decode the last crash dump')
    c.add_argument('project', nargs='?', default='.')
    sub.add_parser('info', parents=[common], help="what's running on the board")
    sub.add_parser('run', parents=[common], help='updater: boot the installed app')
    a = p.parse_args(argv)
    commands = {'flash': cmd_flash, 'monitor': cmd_monitor, 'logs': cmd_logs, 'coredump': cmd_coredump,
                'info': cmd_info, 'run': cmd_run}
    try:
        commands[a.cmd](a)
    except Fail as e:
        print(f'airflash: {e}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == '__main__':
    sys.exit(main())
