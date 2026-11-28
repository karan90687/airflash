#!/usr/bin/env python3
"""airflash laptop tool: flash and monitor an ESP over Wi-Fi (airflash updater + airflash component).

  python airflash.py flash    <project> [--no-build] [--force]   build, check, hand over, upload
  python airflash.py monitor  <project>                          idf.py monitor over Wi-Fi (crash decoding)
  python airflash.py logs                                        plain log stream (no ESP-IDF needed)
  python airflash.py coredump <project>                          download + decode the last crash dump
  python airflash.py info                                        what's running on the board
  python airflash.py run                                         updater: boot the installed app
  python airflash.py find                                        list the airflash boards on this network

Board address: --host <ip> or env AIRFLASH_HOST; else the board remembered in <project>/build/airflash.json
(re-found by its MAC if its IP changed, never swapped for another board); else (nothing remembered yet) the only
board on the network.
Pairing: every board has a token, set when its updater is installed and saved on that laptop in ~/.airflash/token.
Requests prove the token by signing a fresh nonce from the board, so it never crosses the network.
Another laptop: copy ~/.airflash/token, or --token / AIRFLASH_TOKEN.
Standard library only: it runs on the Python that ESP-IDF installs.
"""
import argparse
import concurrent.futures
import hashlib
import hmac
import http.client
import ipaddress
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

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
            cfg = json.load(f)
    except (OSError, ValueError):
        cfg = None  # unknown
    pt_offset = (cfg or {}).get('PARTITION_TABLE_OFFSET')  # an int, or a "0x8000" string
    return {
        'app_bin': os.path.join(desc['build_dir'], desc['app_bin']),
        'elf': os.path.join(desc['build_dir'], desc['app_elf']),
        'partition_table': os.path.join(desc['build_dir'], pt) if pt else None,
        'has_airflash': any(c == 'airflash' or c.endswith('__airflash') for c in desc.get('build_components', [])),
        'coredump': bool(cfg.get('ESP_COREDUMP_ENABLE_TO_FLASH')) if cfg else None,
        'rollback_timeout': (cfg or {}).get('AIRFLASH_ROLLBACK_TIMEOUT_S', 60),  # the new app's network deadline
        'partition_table_offset': int(pt_offset, 0) if isinstance(pt_offset, str) else pt_offset,  # None = unknown
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
    pt_offset = proj.get('partition_table_offset')
    if pt_offset not in (None, 0x8000):  # the app reads the table from its own CONFIG offset: crashes before app_main
        problems.append(f"the app expects its partition table at {pt_offset:#x}, the board's is at 0x8000: "
                        f'menuconfig → Partition Table → Offset of partition table → 0x8000')
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
                        "and it never confirms itself, so the board goes back to the updater at its next reset")
    elif proj.get('coredump') is False:
        warnings.append("crash dumps are off, so a crash won't show where it happened: "
                        "menuconfig → Core dump → Data destination → Flash (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y)")
    return problems, warnings


def coredump_elf_sha(raw):
    """The build ID inside an ESP core dump: the ESP_CORE_DUMP_INFO note = name, u32 version, ELF SHA-256 (hex)."""
    i = raw.find(b'ESP_CORE_DUMP_INFO\0')
    return raw[i + 24:i + 24 + 65].split(b'\0')[0].decode(errors='replace') if i >= 0 else ''


# ---------- what a crash dump says without decoding it ----------

def coredump_log_tail(raw):
    """The component's log ring, copied into the dump: magic AFTAIL01, u32 size, u32 head, u64 total, data[size].
    head = where the next byte goes, so once more than `size` bytes were written the oldest byte is at data[head].
    '' = no tail (the option is off)."""
    i = raw.find(b'AFTAIL01')
    if i < 0 or i + 24 > len(raw):
        return ''
    size, head, total = struct.unpack_from('<IIQ', raw, i + 8)
    data = raw[i + 24:i + 24 + size]
    if len(data) < size or head > size:
        return ''  # cut-off or not really a tail
    return (data[:head] if total < size else data[head:] + data[:head]).decode(errors='replace')


PANIC_STARTS = ('Guru Meditation Error', '***ERROR***', 'abort() was called', 'airflash: not on the network')


def dump_logs(raw, lines=25):
    """The app's last lines before the crash + ESP-IDF's original panic text, indented ('' = none).
    The tail ends with the panic text and the core dump's own "saving" lines; keep the panic, drop those."""
    logs = [l for l in coredump_log_tail(raw).splitlines() if 'esp_core_dump_flash:' not in l]
    start = max((i for i, l in enumerate(logs) if l.startswith(PANIC_STARTS)), default=len(logs))
    return '\n'.join('  ' + line for line in logs[max(0, start - lines):])


def network_abort(raw):
    """A NEW app the component can't get on the network in time aborts on purpose (so it rolls back); the abort
    message is plain text in the dump → (seconds, detail) or None. Printable ASCII only: the C string ends at NUL."""
    m = re.search(rb'airflash: not on the network (\d+) s after boot \(([\x20-\x7e]*)\)', raw)
    return (int(m[1]), m[2].decode()) if m else None


_PASSWORD = ('authentication failed', 'wrong password?')
_SECURITY = ('security mismatch', "the network's security mode isn't accepted (WPA3-only?)")
WIFI_HINTS = {  # ESP-IDF wifi_err_reason_t → (what, likely why)
    2: _PASSWORD, 15: _PASSWORD, 202: _PASSWORD, 204: _PASSWORD, 210: _SECURITY, 211: _SECURITY,
    200: ('connection lost', "lost the router's signal"),
    201: ('network not found', 'wrong SSID, router off, or 5 GHz-only?'),
    212: ('network out of range', 'signal too weak'),
}


def wifi_hint(code, name):
    label, hint = WIFI_HINTS.get(code, (name or 'unknown reason', None))
    return f'{label} (reason {code}): {hint}' if hint else f'{label} (reason {code})'


def explain_abort(seconds, detail):
    """The abort's detail → (what happened, why), shared by `flash` and `coredump`."""
    m = re.match(r'airflash_start\(\) failed \((.*)\)$', detail)  # an esp_err_t name (or "UNKNOWN ERROR")
    if m:  # the app may be on the network fine: airflash itself couldn't run, so nothing could confirm the app
        return (f"the new app's airflash couldn't start ({m[1]}) and the board rolled back",
                'is port 8080 or 3333 already used by the app, or is memory short? (menuconfig → airflash → ports)')
    m = re.match(r"Wi-Fi '(.*)': reason (\d+) (\S*)$", detail)
    if m:
        what, why = f"could not join Wi-Fi '{m[1]}' within {seconds} s", wifi_hint(int(m[2]), m[3])
    elif detail == 'no IP address':
        what, why = (f'joined Wi-Fi but was not on the network within {seconds} s',
                     'the router gave it no IP address (DHCP)')
    elif detail == 'airflash_start() was not called':
        what, why = (f'never called airflash_start() within {seconds} s',
                     'its own network code may be stuck, e.g. wrong Wi-Fi settings')
    else:
        what, why = f'was not on the network within {seconds} s', detail  # a detail this tool doesn't know yet
    return f'the new app {what} and rolled back', why


def rollback_message(raw):
    """Why a just-flashed app fell back to the updater. raw = its crash dump (b'' = there is one but it couldn't
    be downloaded), None = no dump."""
    msg = 'the new app did not confirm itself and the board rolled back to the updater.'
    if raw is None:
        return msg
    logs, why = dump_logs(raw), network_abort(raw)
    if why:
        text = '{}:\n  {}'.format(*explain_abort(*why))
        if why[1].startswith("Wi-Fi '"):
            return text + "\nfix the app's Wi-Fi settings and flash again"
        if why[1] == 'no IP address':  # the app is fine: once the router is, trying it again is enough
            return text + "\ncheck the router's DHCP (address pool full?), then `run` to try it again"
        return text + (f'\nlast logs:\n{logs}' if logs else '') + '\nfix it and flash again'
    if not logs:
        return msg + ' It crashed: run `coredump` to see where.'
    return f'{msg} It crashed.\nlast logs before the crash:\n{logs}\nrun `coredump` for the full decode'


# ---------- pairing ----------

def load_token(arg=''):
    """The pairing token: --token, else AIRFLASH_TOKEN, else ~/.airflash/token (saved when the updater was installed
    from this laptop), else '' (enough for a board without a token)."""
    token = arg or os.environ.get('AIRFLASH_TOKEN', '')
    if not token:
        try:
            token = (Path.home() / '.airflash' / 'token').read_text()
        except OSError:
            return ''
    token = token.strip()
    # printable ASCII 0x21-0x7e, as the updater accepts: a typo would otherwise look like "another laptop"
    if token and not re.fullmatch('[!-~]{1,63}', token):
        raise Fail('the airflash token (--token, AIRFLASH_TOKEN or ~/.airflash/token) must be 1–63 printable '
                   'characters without spaces')
    return token


def sign(token, msg):
    """Hex HMAC-SHA256 keyed with the token: proves we hold it without sending it."""
    return hmac.new(token.encode(), msg.encode(), hashlib.sha256).hexdigest()


def auth_value(token, nonce, method, path, body_sha=None):
    """X-Airflash-Auth: the board's single-use nonce + our signature over it, the method, the path and, for an upload,
    the image's SHA-256 (X-Airflash-Body), so a header seen on the network can't be replayed, reused for another
    request or carry another image."""
    return nonce + ':' + sign(token, '\n'.join((nonce, method, path) + ((body_sha,) if body_sha else ())))


def not_paired(name, token):
    """The board refused us (HTTP 401, or airflash-denied on the log stream)."""
    if not token:
        return Fail('this laptop has no airflash token yet: install the updater from this laptop '
                    '(idf.py flash in updater/) or copy ~/.airflash/token from the laptop that did')
    return Fail(f"{name} was installed from another laptop (or with another token): copy that laptop's "
                f'~/.airflash/token here, or set AIRFLASH_TOKEN')


WARNED = set()  # boards already warned about: resolve(), the command and the log stream may each meet the same one


def open_warning(host, name=None):
    """The warning for a board without a token, once per command (None after the first time)."""
    if host in WARNED:
        return None
    WARNED.add(host)
    return (f'warning: {name or host} has no airflash token: anyone on this network can flash it and read its logs '
            f'(reinstall the updater over USB to set one)')


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


def recv_line(s, buf=b''):
    """One short handshake line → (line, the log bytes already received after it)."""
    while b'\n' not in buf:
        if len(buf) > 80:
            raise ValueError('not an airflash log stream')
        chunk = s.recv(80)
        if not chunk:
            raise OSError('closed')
        buf += chunk
    line, _, rest = buf.partition(b'\n')
    return line.strip(), rest


def board_stream(host, port, token, quiet=5.0, retry=1.0):
    """Yield log bytes forever. The board's buffer survives reboots and keeps its offsets, so after a reconnect
    (network glitch, crash, reboot) we ask for exactly where we stopped: no duplicates, nothing missed.
    The board speaks first with a fresh nonce; we answer with where to resume + our signature of the nonce.
    Silence for `quiet` seconds = reconnect (a crashed board never closes its old connection).
    A board that refuses our token raises Fail: retrying with the same token can't help."""
    log_id, offset, unreachable, said = 0, 0, False, b''
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
                line, rest = recv_line(s)
                hello = line.split()  # airflash-auth <nonce>, or airflash-auth - from a board without a token
                if len(hello) != 2 or hello[0] != b'airflash-auth':
                    if line and line != said:  # e.g. "too many log viewers": say it once, keep retrying
                        said = line
                        yield b'\n[airflash: the board says: ' + line + b']\n'
                    raise ValueError('not an airflash log stream')
                nonce = hello[1].decode()
                if nonce == '-':
                    warn = open_warning(host)
                    if warn:
                        yield f'\n[airflash: {warn}]\n'.encode()
                proof = sign(token, nonce + '\nlog') if token and nonce != '-' else '-'
                s.sendall(f'airflash {log_id:08x} {offset:x} {proof}\n'.encode())
                line, rest = recv_line(s, rest)
                if line == b'airflash-denied':
                    raise not_paired(host, token)
                parts = line.split()
                if len(parts) != 3 or parts[0] != b'airflash':
                    raise ValueError('not an airflash log stream')
                new_id, start = int(parts[1], 16), int(parts[2], 16)
                note = resume_note(log_id, offset, new_id, start)
                if note:
                    yield note.encode()
                log_id, offset = new_id, start
                while True:
                    if rest:
                        offset += len(rest)
                        yield rest
                    rest = s.recv(4096)
                    if not rest:
                        break
            except (OSError, ValueError):
                pass  # silence, reset or garbage: reconnect and resume
        time.sleep(retry)


# ---------- talking to the board ----------

def is_airflash(j):
    """/info JSON from an airflash app or updater (not, say, the app's own web server on port 80)."""
    return isinstance(j, dict) and (j.get('role') == 'app' or 'updater' in j)


class Board:
    def __init__(self, host, token, app_port, log_port, mac=None):
        self.host, self.token, self.app_port, self.log_port = host, token, app_port, log_port
        self.name = host  # until /info tells its name: for messages
        self.mac = mac  # the board we mean, if known: another one that got its old IP doesn't count as it

    def request(self, method, port, path, timeout=3, auth=True):
        headers = self.auth_header(method, port, path) if auth else {}
        conn = http.client.HTTPConnection(self.host, port, timeout=timeout)
        try:
            conn.request(method, path, headers=headers)
            r = conn.getresponse()
            status, body = r.status, r.read()
        finally:
            conn.close()
        if status == 401:
            raise not_paired(self.name, self.token)
        return status, body

    def auth_header(self, method, port, path, body_sha=None):
        """One fresh nonce per protected request (single use, valid 60 s), signed. No token, or no nonce to be had:
        no header, and the board's answer decides (fine for a board without a token, else 401)."""
        if not self.token:
            return {}
        status, nonce = self.request('GET', port, '/nonce', auth=False)
        nonce = nonce.decode(errors='replace').strip()
        if status != 200 or not re.fullmatch('[0-9a-f]{32}', nonce):
            return {}
        return {'X-Airflash-Auth': auth_value(self.token, nonce, method, path, body_sha)}

    def _json(self, port, path):
        try:  # /info is public: no nonce round trip while polling
            status, body = self.request('GET', port, path, timeout=1, auth=False)  # a board that's up: ~0.1 s
            j = json.loads(body) if status == 200 else None
        except (OSError, http.client.HTTPException, ValueError):
            return None
        if not is_airflash(j) or (self.mac and j.get('mac', self.mac) != self.mac):  # another board on its IP
            return None
        self.name = j.get('name') or self.name
        warn = None if j.get('auth') else open_warning(self.host, self.name)
        if warn:
            print(warn, file=sys.stderr)
        return j

    def app_info(self):
        j = self._json(self.app_port, '/info')
        return j if j and j.get('role') == 'app' else None

    def updater_info(self):
        j = self._json(UPDATER_PORT, '/info')
        return j if j and 'updater' in j else None

    def who(self, wait):
        """App and updater share the IP, so ask both. Keep asking while the board (re)joins Wi-Fi. Ask often:
        a crash-looping board is reachable for only a few seconds between crashes."""
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
            time.sleep(0.3)

    def wait_for(self, role, wait):
        deadline = time.time() + wait
        while time.time() < deadline:
            info = self.app_info() if role == 'app' else self.updater_info()
            if info:
                return info
            time.sleep(0.3)
        return None

    def post(self, port, path):
        status, body = self.request('POST', port, path, timeout=10)
        if status != 200:
            raise Fail(f'{path} failed: {status} {body.decode(errors="replace").strip()}')

    def upload(self, path):
        with open(path, 'rb') as f:
            data = f.read()  # hashed and sent from memory: a rebuild during the upload can't make the two differ
        size, sha = len(data), hashlib.sha256(data).hexdigest()
        auth = self.auth_header('POST', UPDATER_PORT, '/update', sha)  # right before the POST: a nonce lives 60 s
        conn = http.client.HTTPConnection(self.host, UPDATER_PORT, timeout=60)
        conn.putrequest('POST', '/update')
        conn.putheader('Content-Type', 'application/octet-stream')
        conn.putheader('Content-Length', str(size))
        conn.putheader('X-Airflash-Body', sha)  # signed in X-Airflash-Auth: the updater checks the bytes it got
        for k, v in auth.items():
            conn.putheader(k, v)
        conn.endheaders()
        start, sent = time.time(), 0
        try:
            for i in range(0, size, 16 * 1024):
                conn.send(data[i:i + 16 * 1024])
                sent = min(i + 16 * 1024, size)
                speed = sent / max(time.time() - start, 0.001) / 1024
                line = f'  uploading {sent * 100 // size:3d}%  {kb(sent)} / {kb(size)}  ({speed:.0f} KB/s)'
                print(f'\r{line:<60}', end='', flush=True)  # pad: \r doesn't erase a longer previous line
        except OSError:
            pass  # the board stopped reading early (e.g. refused our token): its answer below says why
        print('\n  verifying on the board (checksum, SHA-256, chip)...')
        r = conn.getresponse()
        body = r.read().decode(errors='replace').strip()
        conn.close()
        if r.status == 401:
            raise not_paired(self.name, self.token)
        if r.status == 400 and "doesn't match its signature" in body:  # the bytes it got aren't the ones we hashed
            raise Fail('the image changed on the way: retry; if it keeps happening something on the network is '
                       'tampering with uploads')
        if r.status != 200:
            raise Fail(f'the updater refused the image: {body}')


def board_from(a):
    return Board(a.host, a.token, a.app_port, a.log_port, a.mac)


def not_found(a):
    """The error once a command gives up on a.host: what to check, and how to see the real reason."""
    saved = load_board(a.project)
    name = saved.get('name') if saved and saved['host'] == a.host else None
    try:  # the network the board is on (where it was last seen), not necessarily the laptop's
        where = ipaddress.ip_network(f'{a.host}/24', strict=False)
    except ValueError:
        where = network(a.subnet)[0]  # a name like airflash-9220.local
    return Fail(f'{name or a.host} is not answering on {where}. Check:\n'
                f'  - is the board powered?\n'
                f"  - is the router on, with 2.4 GHz enabled? (the ESP can't use 5 GHz)\n"
                f'  - did the Wi-Fi name or password change? then reinstall the updater over USB with the new settings\n'
                f'to see exactly why: plug in USB and run  idf.py -p <port> monitor')


# ---------- finding the board ----------

def entry(host, info):
    """A board as `find` lists it and build/airflash.json remembers it."""
    return {'host': host, 'name': info.get('name'), 'mac': info.get('mac'),
            'role': 'app' if info.get('role') == 'app' else 'updater', 'info': info}


def probe(host, port, timeout):
    """GET /info (public) → (did anything answer?, the JSON if it's an airflash app or updater)."""
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request('GET', '/info')
        r = conn.getresponse()
        j = json.loads(r.read()) if r.status == 200 else None
    except (ConnectionError, http.client.HTTPException, ValueError):
        return True, None  # refused/reset, not HTTP, not JSON: a device, but no airflash on this port
    except OSError:
        return False, None  # timeout, host down/unreachable: no device at this address
    finally:
        conn.close()
    return True, (j if is_airflash(j) else None)


def scan(ips, ports, timeout=1.5):
    """Boards answering /info, sorted by IP. One port at a time, app port first, so a running app wins over
    the updater. Most addresses of a /24 are empty and only time out, so they're asked once, all in parallel."""
    boards, left = {}, list(ips)
    with concurrent.futures.ThreadPoolExecutor(128) as pool:
        for port in ports:
            replies = list(pool.map(lambda ip: probe(ip, port, timeout), left))
            boards.update((ip, entry(ip, j)) for ip, (_, j) in zip(left, replies) if j)
            left = [ip for ip, (answered, j) in zip(left, replies) if answered and not j]
    return sorted(boards.values(), key=lambda b: ipaddress.ip_address(b['host']))


def network(subnet):
    """The addresses to scan: --subnet, else this laptop's /24 (what home routers and phone hotspots hand out)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('10.255.255.255', 1))  # UDP: sends nothing, only makes the OS pick our interface = our IP
        own = s.getsockname()[0]
    except OSError:
        own = None  # not on any network
    finally:
        s.close()
    if not (subnet or own):
        raise Fail('this laptop is not on a network')
    try:
        net = ipaddress.ip_network(subnet or f'{own}/24', strict=False)
    except ValueError as e:
        raise Fail(f'--subnet: {e}')
    ips = [str(ip) for ip in net.hosts()] or [str(net.network_address)]  # older Pythons: a /32 has no hosts()
    return net, [ip for ip in ips if ip != own], own


def mdns_browse(own, group=('224.0.0.251', 5353), timeout=1.5):
    """IPs that answer one mDNS question, "who offers _airflash._tcp?" (both firmwares advertise it). One multicast
    packet, where a scan sends ~250 ARP broadcasts that can deafen a board in Wi-Fi power-save for ~10 s.
    Asked from an ordinary port (a one-shot query, RFC 6762 §6.7), so boards reply straight to us: no need for
    port 5353, which the OS's own mDNS service holds."""
    name = b''.join(bytes([len(p)]) + p for p in (b'_airflash', b'_tcp', b'local')) + b'\0'
    query = struct.pack('>6H', 0, 0, 1, 0, 0, 0) + name + struct.pack('>HH', 12, 1)  # 1 question: PTR, class IN
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ips = set()
    try:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(own))  # out of the Wi-Fi, not a VPN
        s.settimeout(0.2)
        s.sendto(query, group)
        end = time.time() + timeout
        while time.time() < end:
            try:
                ips.add(s.recvfrom(9000)[1][0])
            except socket.timeout:
                pass
    except OSError:
        pass  # no multicast here: the caller falls back to the scan
    finally:
        s.close()
    return ips


def find_boards(a, scan_all=True):
    """mDNS first; knock on every address only if nothing answers (some routers block mDNS) or --subnet asks."""
    net, ips, own = network(a.subnet)
    ports = [a.app_port, UPDATER_PORT]
    if not a.subnet:
        boards = scan(sorted(mdns_browse(own)), ports)  # /info tells who is airflash, as in the scan
        if boards or not scan_all:
            return net, boards
    print(f'scanning every address of {net} (a board in Wi-Fi power-save may stop answering for ~10 s)...',
          file=sys.stderr)
    return net, scan(ips, ports)


def table(boards):
    """IP  NAME  RUNNING  WHAT  MAC  PAIRING, one line per board (PAIRING OFF = no token: anyone can flash it)."""
    def what(b):
        i, app = b['info'], b['info'].get('app', {})
        if b['role'] == 'app':
            return f"{i.get('project')} {i.get('version')} ({i.get('state')})"
        if app.get('installed'):
            return f"{app.get('project')} {app.get('version')} installed ({app.get('state')})"
        return 'no app installed'
    rows = [('IP', 'NAME', 'RUNNING', 'WHAT', 'MAC', 'PAIRING')]
    rows += [(b['host'], b['name'] or '-', b['role'], what(b), b['mac'] or '-',
              'on' if b['info'].get('auth') else 'OFF') for b in boards]
    widths = [max(len(r[i]) for r in rows) for i in range(5)] + [0]
    return '\n'.join('  '.join(c.ljust(w) for c, w in zip(r, widths)) for r in rows)


def load_board(project):
    """The board remembered in build/airflash.json, or None (never saved, or a damaged file)."""
    try:
        with open(os.path.join(project, 'build', 'airflash.json')) as f:
            saved = json.load(f)
        return saved if isinstance(saved, dict) and saved.get('host') else None
    except (OSError, ValueError):
        return None


def remember(project, board):
    """Save it next to the build: per project, never committed. No build/ = not a built project: skip."""
    build = os.path.join(project, 'build')
    if os.path.isdir(build):
        with open(os.path.join(build, 'airflash.json'), 'w') as f:
            json.dump({k: board.get(k) for k in ('host', 'mac', 'name')}, f)


def pick(saved, boards, net):
    """Which board to use after a scan → (board, note). `saved` = the remembered board or None.
    A remembered board is never swapped for another one: `flash` would overwrite that board's app."""
    if saved and saved.get('mac'):
        same = [b for b in boards if b['mac'] == saved['mac']]  # same MAC = same physical board, whatever its IP now
    elif saved:  # remembered without a MAC: only its IP tells, so only a lone board still there counts
        same = boards if len(boards) == 1 and boards[0]['host'] == saved['host'] else []
    else:
        same = boards if len(boards) == 1 else []
    if same:
        b = same[0]
        if saved and b['host'] != saved['host']:
            return b, f"board moved: {saved['host']} → {b['host']}"
        return b, f"using {b['name'] or 'the board'} at {b['host']}"
    if saved:  # e.g. monitor started before the board is powered: wait for it where it was last time
        note = (f"{saved.get('name') or 'the board'} isn't answering (rebooting, crash-looping or off?): "
                f"waiting for it at {saved['host']}")
        if boards:
            others = ', '.join(f"{b['name'] or '?'} ({b['host']})" for b in boards)
            note += (f'\nother boards on the network: {others}; '
                     f'to use another board: --airflash-host <ip> (or delete build/airflash.json)')
        return saved, note
    if boards:
        raise Fail('several airflash boards: pick one with --host <ip>\n' + table(boards))
    raise Fail(f'no airflash board found on {net} (powered? on the same Wi-Fi as this laptop? or --host <ip>)')


def other_network(saved, net, own):
    """The remembered board was last seen outside the network we look on → a note, else None (also for a name)."""
    try:
        if ipaddress.ip_address(saved['host']) in net:
            return None
    except ValueError:
        return None

    def x(ip):
        return '.'.join(str(ip).split('.')[:3]) + '.x'
    return (f"this laptop is on {x(own or net.network_address)}, but {saved.get('name') or saved['host']} "
            f"was last seen on {x(saved['host'])}: is the laptop on the right Wi-Fi?")


def resolve(a):
    """Set a.host if --host/AIRFLASH_HOST didn't: the remembered board if it still answers there with the same MAC,
    else a scan (DHCP can hand the board a new IP; its MAC never changes). Also sets a.mac: the board meant, if
    known."""
    a.mac = None
    if a.host:
        return
    saved = load_board(a.project)
    if saved:
        role, info = Board(saved['host'], a.token, a.app_port, a.log_port).who(wait=3)
        if role and info.get('mac') == saved.get('mac'):  # same MAC = same board (another one may have its old IP)
            a.host, a.mac = saved['host'], saved.get('mac')
            return
    # A remembered board that's silent is usually rebooting or crash-looping: the full scan's ARP flood would
    # deafen it right when it comes back, so then only mDNS (it may have moved), else wait for it where it was.
    net, boards = find_boards(a, scan_all=not saved)
    board, note = pick(saved, boards, net)
    if board is saved:  # about to wait for it: first say if it can't be here (not earlier: it may be routed to us)
        warn = other_network(saved, net, network(a.subnet)[2])
        if warn:
            print(warn, file=sys.stderr)
    print(note, file=sys.stderr)
    a.host, a.mac = board['host'], board.get('mac')  # waiting on its old IP: only its MAC answering counts
    if board is not saved:  # found by the scan: remember it for next time
        remember(a.project, board)


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
        raise not_found(a)

    handed_over = False
    if role == 'updater' and info['updater'].get('running_because') == 'app_crash_loop':
        print('note: the installed app kept crashing right after boot, so the board parked itself in the updater; '
              'flashing the new one')
    if role == 'app':
        print(f"{info['project']} {info['version']} is running → asking it to reboot into the updater")
        board.post(a.app_port, '/reboot-to-updater')
        handed_over = True
        time.sleep(2)
        print('waiting for the updater...')
        info = board.wait_for('updater', 40)
        if info is None:
            raise Fail('the app rebooted but the updater never answered (check the board with `logs`)')
    remember(a.project, entry(a.host, info))

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
    # a new app that isn't on the network by its deadline rolls back: the updater answers only after that + its
    # own reboot and Wi-Fi join (a router with protected management frames makes the rejoin take several seconds)
    deadline = proj['rollback_timeout']
    wait_for_new_app(board, deadline + 40 if deadline else 60)


def wait_for_new_app(board, wait=60):
    """After the upload: the new app answers, or the updater does (rolled back) and its crash dump says why."""
    end, confirm_by = time.time() + wait, None
    while True:
        role, info = board.who(wait=0)  # one round: app port, then updater port
        if role == 'app':
            # the app confirms itself about a second after it's on the network: give it a moment to say valid
            confirm_by = confirm_by or time.time() + 10
            if info['state'] != 'pending_verify' or time.time() > confirm_by:
                print(f"done: {info['project']} {info['version']} is running ({info['state']})")
                return
            time.sleep(1)
            continue
        # right after the upload the updater still answers for a moment before it reboots: only an updater that
        # says the new app failed counts
        if role == 'updater' and info['updater'].get('running_because') in ('app_rolled_back', 'app_crash_loop'):
            updater = info
            break
        if time.time() > end:
            print('uploaded; the app has not answered yet (still joining Wi-Fi?) — check with `info` or `logs`')
            return
        time.sleep(1)
    raw = None
    if updater.get('coredump'):
        try:  # GET only: the dump stays on the board for `coredump` (which also clears it)
            status, raw = board.request('GET', UPDATER_PORT, '/coredump', timeout=30)
        except (OSError, http.client.HTTPException):
            status = 0
        if status != 200:
            raw = b''  # there is a dump, we just couldn't read it: still say it crashed
    raise Fail(rollback_message(raw))


def cmd_monitor(a):
    # The real idf.py monitor (colors, backtrace decoding) reads a local relay instead of the board itself:
    # the relay keeps one resumable board connection, so the monitor survives crashes and reboots like on USB.
    srv = socket.create_server(('127.0.0.1', 0))
    stream = board_stream(a.host, a.log_port, a.token)

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
                except Fail as e:  # the board refused our token: say why in the monitor, then relay nothing more
                    conn.sendall(f'\n[airflash: {e}]\n'.encode())
                    return

    threading.Thread(target=relay, daemon=True).start()
    sys.exit(idf_py(['-p', f'socket://127.0.0.1:{srv.getsockname()[1]}', 'monitor'], a.project, check=False))


def cmd_logs(a):
    out = sys.stdout.buffer
    print(f'log stream from {a.host}:{a.log_port} (Ctrl+C to stop)', file=sys.stderr)
    for chunk in board_stream(a.host, a.log_port, a.token):
        out.write(chunk)
        out.flush()


def cmd_coredump(a):
    proj = read_project(a.project)
    board = board_from(a)
    role, _ = board.who(wait=30)
    if role is None:
        raise not_found(a)
    port = a.app_port if role == 'app' else UPDATER_PORT  # an app that crashed before confirming: the updater has it
    status, raw = board.request('GET', port, '/coredump', timeout=30)
    if status == 404:
        raise Fail('no crash dump on the board (no crash since the last download, or crash dumps off)')
    if status != 200:
        raise Fail(f'/coredump failed: {status}')

    path = os.path.join(a.project, 'build', 'airflash_coredump.bin')
    with open(path, 'wb') as f:
        f.write(raw)
    board.request('DELETE', port, '/coredump')  # the board's copy is a mailbox: something there = a NEW crash
    print(f'saved {path} ({kb(len(raw))}) and cleared it on the board')
    logs, why = dump_logs(raw), network_abort(raw)
    if logs:
        print(f'last logs before the crash (from the dump):\n{logs}')
    if why:  # not a bug in the code: the component gave up because the app never got on the network
        print('{}:\n  {}'.format(*explain_abort(*why)))

    # the dump only holds addresses; the exact build's ELF turns them into file:line, so check it's the same build
    dump_sha = coredump_elf_sha(raw)
    with open(proj['elf'], 'rb') as f:
        elf_sha = hashlib.sha256(f.read()).hexdigest()
    if dump_sha and not elf_sha.startswith(dump_sha):
        raise Fail(f'the dump is from another build (ELF SHA {dump_sha}..., this build {elf_sha[:len(dump_sha)]}...). '
                   f'Decode it with that build: idf.py coredump-info -c {path}')
    idf_py(['coredump-info', '-c', path], a.project, check=False)


RUNNING_BECAUSE = {  # the updater's /info "running_because" → words
    'no_app': 'no app is installed yet',
    'requested': 'it was asked to (a `flash` hand-over, the recovery pin or a USB erase)',
    'app_rolled_back': 'the new app crashed or failed before confirming itself, so the board rolled back',
    'app_crash_loop': 'the app crashed several times in a row right after boot, so the board parked itself in the '
                      'updater: run `coredump` to see where, then fix and flash (or `run` to try it again)',
}


def cmd_info(a):
    role, info = board_from(a).who(wait=15)
    if role is None:
        raise not_found(a)
    remember(a.project, entry(a.host, info))
    dump = info.get('coredump')
    dump_line = f'crash dump waiting ({kb(dump)}): run `coredump`' if dump else 'no crash dump'
    pairing = 'pairing: on' if info.get('auth') else 'pairing: OFF (anyone on this network can flash it)'
    if role == 'app':
        print(f"app running: {info['project']} {info['version']} (IDF {info['idf']}), state {info['state']}, "
              f"airflash {info['airflash']}, logs on port {info['log_port']}")
        print(f"boot #{info['boot']}, last reset: {info['reset_reason']}, {dump_line}")
        print(pairing)
        return
    u, c, app = info['updater'], info['chip'], info['app']
    why = u['running_because']
    print(f"updater {u['version']} (reset: {u['reset_reason']}) running because {RUNNING_BECAUSE.get(why, why)}")
    print(dump_line)
    print(pairing)
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


def cmd_find(a):
    net, boards = find_boards(a)
    if not boards:
        raise Fail(f'no airflash board found on {net} (powered? on the same Wi-Fi as this laptop? '
                   f'a bigger network: --subnet, e.g. 10.0.0.0/22)')
    print(table(boards))
    if len(boards) == 1 and not load_board(a.project):  # only a first board: never replace the project's own
        remember(a.project, boards[0])


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
    # Piped (into a file, tee, an IDE), Python would hold our normal lines back while errors and idf.py's own
    # output go straight through, scrambling the order: flush at every line instead.
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(line_buffering=True)
    common = argparse.ArgumentParser(add_help=False)  # options accepted after every command
    common.add_argument('project', nargs='?', default='.', help='ESP-IDF project folder (default: .)')
    common.add_argument('--host', default=os.environ.get('AIRFLASH_HOST') or None,
                        help='board IP or name (default: the remembered board, else a network scan)')
    common.add_argument('--subnet', help="network to scan (default: this laptop's /24)")
    common.add_argument('--token', default='',
                        help='pairing token (default: AIRFLASH_TOKEN, else ~/.airflash/token)')
    common.add_argument('--app-port', type=int, default=8080)
    common.add_argument('--log-port', type=int, default=3333)

    p = argparse.ArgumentParser(prog='airflash', description='Flash and monitor an ESP over Wi-Fi.')
    sub = p.add_subparsers(dest='cmd', required=True)
    f = sub.add_parser('flash', parents=[common], help='build, check, hand over and upload')
    f.add_argument('--no-build', action='store_true', help='upload the existing build')
    f.add_argument('--force', action='store_true', help='flash even if the compatibility checks fail')
    sub.add_parser('monitor', parents=[common], help='idf.py monitor over Wi-Fi')
    sub.add_parser('logs', parents=[common], help='plain log stream')
    sub.add_parser('coredump', parents=[common], help='download and decode the last crash dump')
    sub.add_parser('info', parents=[common], help="what's running on the board")
    sub.add_parser('run', parents=[common], help='updater: boot the installed app')
    sub.add_parser('find', parents=[common], help='list the airflash boards on this network')
    a = p.parse_args(argv)
    commands = {'flash': cmd_flash, 'monitor': cmd_monitor, 'logs': cmd_logs, 'coredump': cmd_coredump,
                'info': cmd_info, 'run': cmd_run, 'find': cmd_find}
    try:
        a.token = load_token(a.token)
        if a.cmd != 'find':
            resolve(a)  # once, before the command: every command then talks to a.host
        commands[a.cmd](a)
    except Fail as e:
        print(f'airflash: {e}', file=sys.stderr)
        return 1
    except (OSError, http.client.HTTPException) as e:
        print(f'airflash: {type(e).__name__}: {e} (if the board is rebooting or crash-looping, run it again)',
              file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == '__main__':
    sys.exit(main())
