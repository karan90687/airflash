"""Checks the binary parsers, the compatibility rules and pairing. Run: python test_airflash.py"""
import argparse
import contextlib
import hashlib
import hmac
import io
import json
import os
import struct
import tempfile

import airflash


def fake_app(chip_id=9, min_rev=0, max_rev=99, project=b'demo', size=4096):
    head = bytearray(size)
    head[0] = 0xE9
    struct.pack_into('<H', head, 12, chip_id)
    struct.pack_into('<HH', head, 15, min_rev, max_rev)
    struct.pack_into('<I', head, 0x20, 0xABCD5432)
    head[0x30:0x31] = b'1'
    head[0x50:0x50 + len(project)] = project
    head[0x90:0x96] = b'v5.4.3'
    return bytes(head)


def fake_table(*entries):
    data = b''
    for label, ptype, subtype, offset, size in entries:
        data += struct.pack('<HBBII16sI', 0x50AA, ptype, subtype, offset, size, label.encode(), 0)
    return data + b'\xeb\xeb' + b'\xff' * 30 + b'\xff' * 32


def write(tmp, name, data):
    path = os.path.join(tmp, name)
    with open(path, 'wb') as f:
        f.write(data)
    return path


BOARD = {
    'chip': {'target': 'esp32s3', 'image_chip_id': 9, 'revision': 2},
    'app': {'slot_size': 5 * 1024 * 1024},
    'partitions': [{'label': 'nvs', 'type': 1, 'subtype': 2, 'size': 0x6000},
                   {'label': 'storage', 'type': 1, 'subtype': 0x82, 'size': 0x100000}],
}

with tempfile.TemporaryDirectory() as tmp:
    img = airflash.read_app_image(write(tmp, 'app.bin', fake_app()))
    assert (img['chip_id'], img['min_rev'], img['max_rev'], img['project'], img['idf']) == (9, 0, 99, 'demo', 'v5.4.3')

    table = write(tmp, 'pt.bin', fake_table(('nvs', 1, 2, 0x9000, 0x6000), ('factory', 0, 0, 0x10000, 0x100000),
                                            ('storage', 1, 0x82, 0x110000, 0xF0000)))
    parts = airflash.read_partition_table(table)
    assert [p['label'] for p in parts] == ['nvs', 'factory', 'storage'] and parts[2]['size'] == 0xF0000

    proj = {'partition_table': table, 'has_airflash': True}
    assert airflash.check_compatibility(img, proj, BOARD) == ([], [])

    wrong_chip = airflash.read_app_image(write(tmp, 'c3.bin', fake_app(chip_id=5)))
    assert 'esp32c3' in airflash.check_compatibility(wrong_chip, proj, BOARD)[0][0]

    new_rev = airflash.read_app_image(write(tmp, 'rev.bin', fake_app(min_rev=100, max_rev=199)))
    assert 'v1.0' in airflash.check_compatibility(new_rev, proj, BOARD)[0][0]

    big = dict(img, size=6 * 1024 * 1024)
    assert 'slot' in airflash.check_compatibility(big, proj, BOARD)[0][0]

    missing = write(tmp, 'pt2.bin', fake_table(('fatfs', 1, 0x81, 0x110000, 0x10000)))
    assert "'fatfs'" in airflash.check_compatibility(img, dict(proj, partition_table=missing), BOARD)[0][0]

    assert airflash.check_compatibility(img, dict(proj, has_airflash=False), BOARD)[1]  # warns, doesn't block
    assert 'crash dumps are off' in airflash.check_compatibility(img, dict(proj, coredump=False), BOARD)[1][0]
    assert airflash.check_compatibility(img, dict(proj, partition_table_offset=0x9000), BOARD)[0] == [
        "the app expects its partition table at 0x9000, the board's is at 0x8000: "
        'menuconfig → Partition Table → Offset of partition table → 0x8000']
    assert airflash.check_compatibility(img, dict(proj, partition_table_offset=0x8000), BOARD) == ([], [])

    # read_project: the partition table offset from sdkconfig.json, an int or a "0x..." string, else unknown
    build = os.path.join(tmp, 'p', 'build')
    os.makedirs(os.path.join(build, 'config'))
    desc = {'build_dir': build, 'app_bin': 'a.bin', 'app_elf': 'a.elf'}
    write(build, 'project_description.json', json.dumps(desc).encode())
    write(build, 'flasher_args.json', b'{}')
    for value, want in ((0x9000, 0x9000), ('0x8000', 0x8000)):
        write(os.path.join(build, 'config'), 'sdkconfig.json', json.dumps({'PARTITION_TABLE_OFFSET': value}).encode())
        assert airflash.read_project(os.path.join(tmp, 'p'))['partition_table_offset'] == want
    os.remove(os.path.join(build, 'config', 'sdkconfig.json'))
    assert airflash.read_project(os.path.join(tmp, 'p'))['partition_table_offset'] is None

# the build ID inside a core dump: note name (20 bytes, padded) + u32 version + ELF SHA as text
blob = b'junk' + b'ESP_CORE_DUMP_INFO\0\0' + struct.pack('<I', 2) + b'abcdef012\0' + b'more'
assert airflash.coredump_elf_sha(blob) == 'abcdef012' and airflash.coredump_elf_sha(b'nothing') == ''


def fails(fn, *args):
    try:
        fn(*args)
    except airflash.Fail as e:
        return str(e)
    raise AssertionError('expected Fail')


# pairing: the header for one request (same vector as the firmware's) and where the token comes from
TOKEN, NONCE = '00112233445566778899aabbccddeeff', '0123456789abcdef0123456789abcdef'
expected = hmac.new(TOKEN.encode(), (NONCE + '\nPOST\n/update').encode(), hashlib.sha256).hexdigest()
assert expected == '353e8fa658d26de92fed45ec56838a74ff53f6be6fe63f74fa757eb07ae500f6'  # regression
assert airflash.auth_value(TOKEN, NONCE, 'POST', '/update') == NONCE + ':' + expected
# an upload also signs the image's SHA-256 (X-Airflash-Body) as a 4th line: same vector as the updater's
hello = hashlib.sha256(b'hello').hexdigest()
expected = hmac.new(TOKEN.encode(), f'{NONCE}\nPOST\n/update\n{hello}'.encode(), hashlib.sha256).hexdigest()
assert expected == 'e3d8eba5c8baafd90d87afe824183cdf01c66e5df91c9984d83a106f9f11e7bc'  # regression
assert airflash.auth_value(TOKEN, NONCE, 'POST', '/update', hello) == NONCE + ':' + expected
TAMPERED = ('the image changed on the way: retry; if it keeps happening something on the network is tampering '
            'with uploads')
OTHER_LAPTOP = ("%s was installed from another laptop (or with another token): copy that laptop's "
                '~/.airflash/token here, or set AIRFLASH_TOKEN')
NO_TOKEN = ('this laptop has no airflash token yet: install the updater from this laptop (idf.py flash in updater/) '
            'or copy ~/.airflash/token from the laptop that did')

env = {k: os.environ.get(k) for k in ('HOME', 'USERPROFILE', 'AIRFLASH_TOKEN')}
with tempfile.TemporaryDirectory() as home:
    os.environ['HOME'] = os.environ['USERPROFILE'] = home  # Path.home(): HOME, or USERPROFILE on Windows
    os.environ.pop('AIRFLASH_TOKEN', None)
    assert airflash.load_token() == ''  # none
    os.mkdir(os.path.join(home, '.airflash'))
    write(os.path.join(home, '.airflash'), 'token', b'a' * 32 + b'\n')
    assert airflash.load_token() == 'a' * 32  # the file, whitespace stripped
    os.environ['AIRFLASH_TOKEN'] = 'b' * 32
    assert airflash.load_token() == 'b' * 32  # env beats the file
    assert airflash.load_token('c' * 32) == 'c' * 32  # --token beats env
    assert airflash.load_token('My-own_token!~') == 'My-own_token!~'  # any printable ASCII, not only hex
    assert airflash.load_token('x' * 63) == 'x' * 63
    bad = ('the airflash token (--token, AIRFLASH_TOKEN or ~/.airflash/token) must be 1–63 printable characters '
           'without spaces')
    assert fails(airflash.load_token, 'not a token') == bad and fails(airflash.load_token, 'x' * 64) == bad
    assert fails(airflash.load_token, 'café') == bad  # not ASCII
for k, v in env.items():
    if v is None:
        os.environ.pop(k, None)
    else:
        os.environ[k] = v

# log stream: the fake board speaks first (airflash-auth <nonce>), reads our answer (where to resume + our
# signature of the nonce), replies with what it has (or airflash-denied), then the log bytes
import socket, threading  # noqa: E401,E402


def fake_log_board(script):
    """One connection per (nonce, reply) → (port, the answer lines the client sent)."""
    srv = socket.create_server(('127.0.0.1', 0))
    answers = []

    def run():
        for nonce, reply in script:
            conn, _ = srv.accept()
            with conn:
                conn.sendall(b'airflash-auth ' + nonce + b'\n')
                line = b''
                while not line.endswith(b'\n'):
                    line += conn.recv(200)
                answers.append(line.decode())
                conn.sendall(reply)
    threading.Thread(target=run, daemon=True).start()
    return srv.getsockname()[1], answers


def proof(nonce):
    return hmac.new(TOKEN.encode(), nonce + b'\nlog', hashlib.sha256).hexdigest()


def read_until(stream, end):
    got = b''
    while end not in got:
        got += next(stream)
    return got


n1, n2, n3 = b'1' * 32, b'2' * 32, b'3' * 32
port, answers = fake_log_board([(n1, b'airflash 0000abcd 00000000\nline1\n'),          # log 0xabcd from offset 0
                                (n2, b'airflash 0000abcd 0000000000000006\nline2\n'),  # resumed where we stopped
                                (n3, b'airflash 00001234 0\nboot2\n')])                 # buffer started over (new id)
got = read_until(airflash.board_stream('127.0.0.1', port, TOKEN, quiet=1, retry=0.01), b'boot2')
assert answers == [f'airflash 00000000 0 {proof(n1)}\n', f'airflash 0000abcd 6 {proof(n2)}\n',
                   f'airflash 0000abcd c {proof(n3)}\n'], answers
assert got.count(b'line1') == 1 and got.count(b'line2') == 1 and b'no airflash token' not in got
assert got.index(b'started over') < got.index(b'boot2')

# a board without a token: '-' both ways, and the warning once per command, not per reconnect
airflash.WARNED.clear()
port, answers = fake_log_board([(b'-', b'airflash 00000001 0\nhi\n'), (b'-', b'airflash 00000001 3\nmore\n')])
got = read_until(airflash.board_stream('127.0.0.1', port, TOKEN, quiet=1, retry=0.01), b'more')
assert answers == ['airflash 00000000 0 -\n', 'airflash 00000001 3 -\n'], answers
assert got.count(b'warning: 127.0.0.1 has no airflash token: anyone on this network') == 1, got

# refused: a Fail on the first refusal (retrying with the same token can't help), with or without a token here
port, answers = fake_log_board([(n1, b'airflash-denied\n'), (n2, b'airflash-denied\n')])
assert fails(next, airflash.board_stream('127.0.0.1', port, 'f' * 32, retry=0.01)) == OTHER_LAPTOP % '127.0.0.1'
assert fails(next, airflash.board_stream('127.0.0.1', port, '', retry=0.01)) == NO_TOKEN
assert answers[1] == 'airflash 00000000 0 -\n', answers


# picking a board after a scan (pure, no network)


def found(host, mac, name='airflash-x'):
    return airflash.entry(host, {'role': 'app', 'project': 'demo', 'version': '1', 'state': 'valid',
                                 'name': name, 'mac': mac})


NET = '10.42.0.0/24'
saved = {'host': '10.42.0.104', 'mac': 'aa', 'name': 'airflash-9220'}
board, note = airflash.pick(saved, [found('10.42.0.7', 'bb'), found('10.42.0.131', 'aa')], NET)
assert board['host'] == '10.42.0.131' and note == 'board moved: 10.42.0.104 → 10.42.0.131', note
assert airflash.pick(saved, [found('10.42.0.104', 'aa')], NET)[1] == 'using airflash-x at 10.42.0.104'
assert airflash.pick(None, [found('10.42.0.7', 'bb')], NET)[1] == 'using airflash-x at 10.42.0.7'
several = fails(airflash.pick, None, [found('10.42.0.7', 'bb'), found('10.42.0.8', 'cc')], NET)
assert 'several' in several and '10.42.0.8' in several
assert airflash.pick(saved, [], NET) == (saved, "airflash-9220 isn't answering (rebooting, crash-looping or off?): "
                                                'waiting for it at 10.42.0.104')  # nothing answers: wait for it
# the remembered board is silent and another one answers: wait for ours, never switch to that one
board, note = airflash.pick(saved, [found('10.42.0.7', 'bb', 'airflash-b')], NET)
assert board is saved and note.endswith('waiting for it at 10.42.0.104\nother boards on the network: '
                                        'airflash-b (10.42.0.7); to use another board: --airflash-host <ip> '
                                        '(or delete build/airflash.json)'), note
assert airflash.pick(saved, [found('10.42.0.7', 'bb'), found('10.42.0.8', 'cc')], NET)[0] is saved
assert 'no airflash board found on 10.42.0.0/24' in fails(airflash.pick, None, [], NET)
old = {'host': '10.42.0.104', 'mac': None, 'name': None}  # no MAC remembered: only a lone board on its IP is it
assert airflash.pick(old, [found('10.42.0.104', 'aa')], NET)[0]['host'] == '10.42.0.104'
assert airflash.pick(old, [found('10.42.0.131', 'aa')], NET)[0] is old
assert airflash.pick(old, [found('10.42.0.104', 'aa'), found('10.42.0.8', 'cc')], NET)[0] is old

net, ips, _ = airflash.network('192.0.2.9/32')
assert ips == ['192.0.2.9'] and len(airflash.network('192.0.2.0/24')[1]) == 254

# scan against fake boards on 127.0.0.1: one board per IP, so each port set gives at most one
import http.server  # noqa: E402
asked = []


def serve(status, body):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            asked.append(self.path)
            self.send_response(status)
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass
    srv = http.server.HTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1]


app = serve(200, b'{"role":"app","project":"demo","version":"1.0","state":"valid","name":"airflash-9220",'
                 b'"mac":"cc:ba:97:0e:92:20","auth":true}')
updater = serve(200, b'{"updater":{"version":"0.1.0"},"mac":"cc:ba:97:0e:92:20","app":{"installed":false}}')
web = serve(200, b'<html>the app\'s own web server</html>')
missing = serve(404, b'not found')
silent = socket.create_server(('127.0.0.1', 0))  # accepts connections, never answers: like a sleeping/absent host

b = airflash.scan(['127.0.0.1'], [app, updater, web])
assert [(x['role'], x['name'], x['mac']) for x in b] == [('app', 'airflash-9220', 'cc:ba:97:0e:92:20')], b
assert asked == ['/info']  # the app answered on the first port: the updater port isn't asked
assert airflash.table(b).splitlines()[1].endswith('  on')  # PAIRING column
b = airflash.scan(['127.0.0.1'], [missing, web, updater])
assert [(x['role'], x['name'], x['mac']) for x in b] == [('updater', None, 'cc:ba:97:0e:92:20')], b
assert 'no app installed' in airflash.table(b) and airflash.table(b).endswith('  OFF')  # no "auth": open board
assert airflash.scan(['127.0.0.1'], [missing, web]) == []
# waiting for a board on its old IP: another board that got that IP isn't it (its MAC differs)
assert airflash.Board('127.0.0.1', TOKEN, app, 3333, 'aa:aa:aa:aa:aa:aa').app_info() is None
assert airflash.Board('127.0.0.1', TOKEN, app, 3333, 'cc:ba:97:0e:92:20').app_info()
assert airflash.scan(['127.0.0.1'], [silent.getsockname()[1], app], timeout=0.2) == []  # timed out: skipped

# a board without a token: one warning per command, however often it's asked
airflash.WARNED.clear()
err, open_app = io.StringIO(), serve(200, b'{"role":"app","name":"airflash-open","auth":false}')
with contextlib.redirect_stderr(err):
    board = airflash.Board('127.0.0.1', TOKEN, open_app, 3333)
    assert board.app_info() and board.app_info()
assert err.getvalue() == ('warning: airflash-open has no airflash token: anyone on this network can flash it and '
                          'read its logs (reinstall the updater over USB to set one)\n'), err.getvalue()

# remembered board: build/airflash.json, only in a built project, damaged file = nothing remembered
with tempfile.TemporaryDirectory() as proj:
    airflash.remember(proj, found('10.42.0.104', 'aa'))
    assert airflash.load_board(proj) is None
    os.mkdir(os.path.join(proj, 'build'))
    airflash.remember(proj, found('10.42.0.104', 'aa', 'airflash-9220'))
    assert airflash.load_board(proj) == saved
    write(os.path.join(proj, 'build'), 'airflash.json', b'{damaged')
    assert airflash.load_board(proj) is None

# `find` remembers a lone board only if the project has none yet: it never replaces the board it flashes
real_find = airflash.find_boards
airflash.find_boards = lambda a: (NET, [found('10.42.0.7', 'bb')])
with tempfile.TemporaryDirectory() as proj, contextlib.redirect_stdout(io.StringIO()):
    os.mkdir(os.path.join(proj, 'build'))
    airflash.cmd_find(argparse.Namespace(project=proj))
    assert airflash.load_board(proj)['mac'] == 'bb'  # nothing remembered yet: remembered
    airflash.remember(proj, saved)
    airflash.cmd_find(argparse.Namespace(project=proj))
    assert airflash.load_board(proj) == saved  # a different board is remembered: unchanged
airflash.find_boards = real_find

# mDNS: a fake responder on localhost gets our one question and answers it, like a board on the Wi-Fi would
responder = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
responder.bind(('127.0.0.1', 0))
questions = []


def answer():
    q, sender = responder.recvfrom(512)
    questions.append(q)
    responder.sendto(b'any reply: only the sender matters', sender)


threading.Thread(target=answer, daemon=True).start()
assert airflash.mdns_browse('127.0.0.1', responder.getsockname(), timeout=0.5) == {'127.0.0.1'}
assert questions[0] == b'\0\0\0\0\0\1\0\0\0\0\0\0\x09_airflash\x04_tcp\x05local\0\0\x0c\0\1'  # 1 question, PTR, IN


# log tail inside a dump: AFTAIL01, u32 size, u32 head, u64 total, data[size]
def tail(size, head, total, data):
    return b'dump...' + b'AFTAIL01' + struct.pack('<IIQ', size, head, total) + data + b'...more dump'


assert airflash.coredump_log_tail(tail(16, 6, 6, b'boot1\n'.ljust(16, b'\0'))) == 'boot1\n'  # not full yet
logs = b'1\n2\n3\n4\n'  # 20 bytes written into an 8-byte ring: the next write goes at 3, so the oldest byte is there
assert airflash.coredump_log_tail(tail(8, 3, 20, logs[-3:] + logs[:-3])) == '1\n2\n3\n4\n'
assert airflash.coredump_log_tail(b'no tail here') == '' and airflash.dump_logs(b'') == ''
assert airflash.dump_logs(tail(8, 3, 20, logs[-3:] + logs[:-3]), lines=2) == '  3\n  4'
# a crash: N app lines before the panic text + the panic text itself, without the dump's own "saving" lines
crashed = (b'a1\na2\na3\nGuru Meditation Error: Core  0 panic\'ed (LoadProhibited).\nBacktrace: 0x42:0x3f\n'
           b'I (10659) esp_core_dump_flash: Save core dump to flash...\n')
assert airflash.dump_logs(tail(len(crashed), 0, len(crashed), crashed), lines=2) == \
    "  a2\n  a3\n  Guru Meditation Error: Core  0 panic'ed (LoadProhibited).\n  Backtrace: 0x42:0x3f"

# the component's "not on the network" abort, as plain text in the dump
abort = b"\0\0airflash: not on the network 60 s after boot (%s)\0garbage)"
assert airflash.network_abort(abort % b"Wi-Fi 'my (home)': reason 201 NO_AP_FOUND") == \
    (60, "Wi-Fi 'my (home)': reason 201 NO_AP_FOUND")
assert airflash.network_abort(b'a real crash') is None
wifi = "Wi-Fi 'x': reason %d %s"
assert airflash.explain_abort(60, wifi % (201, 'NO_AP_FOUND')) == \
    ("the new app could not join Wi-Fi 'x' within 60 s and rolled back",
     'network not found (reason 201): wrong SSID, router off, or 5 GHz-only?')
assert airflash.explain_abort(9, wifi % (15, '4WAY_HANDSHAKE_TIMEOUT'))[1] == \
    'authentication failed (reason 15): wrong password?'
assert 'WPA3-only?' in airflash.explain_abort(9, wifi % (211, 'NO_AP_FOUND_IN_AUTHMODE_THRESHOLD'))[1]
assert airflash.explain_abort(9, wifi % (212, 'NO_AP_FOUND_IN_RSSI_THRESHOLD'))[1].endswith('signal too weak')
assert airflash.explain_abort(9, wifi % (200, 'BEACON_TIMEOUT'))[1].endswith("lost the router's signal")
assert airflash.explain_abort(9, wifi % (205, 'CONNECTION_FAIL'))[1] == 'CONNECTION_FAIL (reason 205)'
assert airflash.explain_abort(9, 'no IP address')[1] == 'the router gave it no IP address (DHCP)'
assert airflash.explain_abort(9, 'airflash_start() was not called')[0] == \
    'the new app never called airflash_start() within 9 s and rolled back'

# after an upload: the message for each way the new app can fall back to the updater
rolled = 'the new app did not confirm itself and the board rolled back to the updater.'
assert airflash.rollback_message(None) == rolled  # no dump
assert airflash.rollback_message(b'') == rolled + ' It crashed: run `coredump` to see where.'
crash = airflash.rollback_message(tail(8, 3, 20, logs[-3:] + logs[:-3]))
assert crash == rolled + ' It crashed.\nlast logs before the crash:\n  1\n  2\n  3\n  4\nrun `coredump` for the full decode'
stuck = airflash.rollback_message(abort % b'airflash_start() was not called' + tail(16, 6, 6, b'boot1\n'.ljust(16, b'\0')))
assert stuck.startswith('the new app never called airflash_start() within 60 s and rolled back:\n') and \
    stuck.endswith('\nlast logs:\n  boot1\nfix it and flash again'), stuck
assert '`run` to try it again' in airflash.rollback_message(abort % b'no IP address')
# the component itself couldn't start (its esp_err_t name inside the detail's parentheses)
no_mem = abort % b'airflash_start() failed (ESP_ERR_NO_MEM)'
assert airflash.network_abort(no_mem) == (60, 'airflash_start() failed (ESP_ERR_NO_MEM)')
start_failed = ("the new app's airflash couldn't start (ESP_ERR_NO_MEM) and the board rolled back:\n"
                '  is port 8080 or 3333 already used by the app, or is memory short? (menuconfig → airflash → ports)\n')
assert airflash.rollback_message(no_mem) == start_failed + 'fix it and flash again'
assert airflash.rollback_message(no_mem + tail(16, 6, 6, b'boot1\n'.ljust(16, b'\0'))) == \
    start_failed + 'last logs:\n  boot1\nfix it and flash again'

# a remembered board last seen on another network than the laptop's
import ipaddress  # noqa: E402
here = ipaddress.ip_network('10.0.1.0/24')
assert airflash.other_network(saved, here, '10.0.1.5') == \
    'this laptop is on 10.0.1.x, but airflash-9220 was last seen on 10.42.0.x: is the laptop on the right Wi-Fi?'
assert 'but 10.42.0.104 was' in airflash.other_network(dict(saved, name=None), here, None)  # and no own IP
assert airflash.other_network(saved, ipaddress.ip_network(NET), '10.42.0.5') is None
assert airflash.other_network({'host': 'board.local'}, here, '10.0.1.5') is None  # a name: can't tell

# giving up on a board: named after the remembered board if it's the one we tried
with tempfile.TemporaryDirectory() as proj:
    os.mkdir(os.path.join(proj, 'build'))
    airflash.remember(proj, saved)
    msg = str(airflash.not_found(argparse.Namespace(project=proj, host='10.42.0.104', subnet=NET)))
    assert msg == ('airflash-9220 is not answering on 10.42.0.0/24. Check:\n'
                   '  - is the board powered?\n'
                   "  - is the router on, with 2.4 GHz enabled? (the ESP can't use 5 GHz)\n"
                   '  - did the Wi-Fi name or password change? then reinstall the updater over USB with the new settings\n'
                   'to see exactly why: plug in USB and run  idf.py -p <port> monitor'), msg
    assert str(airflash.not_found(argparse.Namespace(project=proj, host='10.42.0.9', subnet=NET))).startswith(
        '10.42.0.9 is not answering on 10.42.0.0/24.')

# a fake updater after a flash: the new app never answers, the updater has a dump saying Wi-Fi failed
seen = []


class FakeUpdater(http.server.BaseHTTPRequestHandler):
    routes = {'/info': b'{"updater":{"version":"0.4","running_because":"app_rolled_back"},"coredump":4096,'
                       b'"app":{"installed":true,"state":"aborted"},"auth":true}',
              '/coredump': abort % b"Wi-Fi 'my-wifi': reason 201 NO_AP_FOUND"}

    def do_GET(self):
        seen.append((self.command, self.path))
        self.send_response(200 if self.path in self.routes else 404)
        self.end_headers()
        self.wfile.write(self.routes.get(self.path, b''))
    do_DELETE = do_GET

    def log_message(self, *args):
        pass


fake = http.server.HTTPServer(('127.0.0.1', 0), FakeUpdater)
threading.Thread(target=fake.serve_forever, daemon=True).start()
airflash.UPDATER_PORT = fake.server_address[1]
msg = fails(airflash.wait_for_new_app, airflash.Board('127.0.0.1', '', missing, 3333), 0.3)
airflash.UPDATER_PORT = 80
assert msg == ("the new app could not join Wi-Fi 'my-wifi' within 60 s and rolled back:\n"
               '  network not found (reason 201): wrong SSID, router off, or 5 GHz-only?\n'
               "fix the app's Wi-Fi settings and flash again"), msg
assert ('GET', '/coredump') in seen and ('DELETE', '/coredump') not in seen  # left on the board for `coredump`

# right after an upload the updater answers for a moment before it reboots: that's not a rollback
FakeUpdater.routes['/info'] = b'{"updater":{"running_because":"requested"},"coredump":0,"app":{"installed":true},' \
                              b'"auth":true}'
airflash.UPDATER_PORT = fake.server_address[1]
assert airflash.wait_for_new_app(airflash.Board('127.0.0.1', '', missing, 3333), 0.3) is None  # waits, no Fail
airflash.UPDATER_PORT = 80


# a paired board over HTTP: each protected request = GET /nonce, then the request with X-Airflash-Auth;
# an upload also signs its body's SHA-256 (X-Airflash-Body), which the updater checks against the bytes it got
class PairedBoard(http.server.BaseHTTPRequestHandler):
    nonces, done, tamper = set(), [], False

    def do_GET(self):
        if self.path == '/nonce':
            nonce = os.urandom(16).hex()
            self.nonces.add(nonce)
            return self.reply(200, nonce.encode())
        if self.path == '/info':
            return self.reply(200, b'{"role":"app","name":"airflash-9220","auth":true}')
        self.protected()

    def protected(self):
        body = self.rfile.read(int(self.headers.get('Content-Length') or 0))
        if self.tamper:
            body = b'X' + body[1:]  # what something on the network could do to the image
        nonce, _, mac = (self.headers.get('X-Airflash-Auth') or '').partition(':')
        signed = f'{nonce}\n{self.command}\n{self.path}'
        if self.path == '/update':
            signed += '\n' + (self.headers.get('X-Airflash-Body') or '')
        good = hmac.new(TOKEN.encode(), signed.encode(), hashlib.sha256).hexdigest()
        ok = nonce in self.nonces and hmac.compare_digest(mac, good)
        self.nonces.discard(nonce)  # single use
        if not ok:
            return self.reply(401, b'airflash: not paired: this board expects another airflash token')
        if self.path == '/update' and hashlib.sha256(body).hexdigest() != self.headers.get('X-Airflash-Body'):
            return self.reply(400, b"airflash: the upload doesn't match its signature")
        self.done.append((self.command, self.path))
        self.reply(200, b'OK')
    do_POST = do_DELETE = protected

    def reply(self, status, body):
        self.send_response(status)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


paired = http.server.HTTPServer(('127.0.0.1', 0), PairedBoard)
threading.Thread(target=paired.serve_forever, daemon=True).start()
pp = paired.server_address[1]
airflash.UPDATER_PORT = pp
with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):  # quiet upload progress
    image = write(tmp, 'app.bin', fake_app())
    board = airflash.Board('127.0.0.1', TOKEN, pp, 3333)
    board.post(pp, '/reboot-to-updater')
    assert board.request('GET', pp, '/coredump')[0] == 200 and board.request('DELETE', pp, '/coredump')[0] == 200
    board.upload(image)
    assert PairedBoard.done == [('POST', '/reboot-to-updater'), ('GET', '/coredump'), ('DELETE', '/coredump'),
                                ('POST', '/update')], PairedBoard.done
    PairedBoard.tamper = True
    assert fails(board.upload, image) == TAMPERED  # signed for other bytes than the updater got
    PairedBoard.tamper = False
    wrong = airflash.Board('127.0.0.1', 'f' * 32, pp, 3333)
    assert fails(wrong.post, pp, '/run') == OTHER_LAPTOP % '127.0.0.1'  # name unknown yet: the IP
    assert fails(wrong.upload, image) == OTHER_LAPTOP % '127.0.0.1'
    assert wrong.app_info() and fails(wrong.post, pp, '/run') == OTHER_LAPTOP % 'airflash-9220'  # /info named it
    assert fails(airflash.Board('127.0.0.1', '', pp, 3333).post, pp, '/run') == NO_TOKEN
airflash.UPDATER_PORT = 80
assert len(PairedBoard.done) == 4 and not PairedBoard.nonces  # every nonce used once, refused ones too

print('ok')
