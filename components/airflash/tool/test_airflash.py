"""Checks the binary parsers and the compatibility rules. Run: python test_airflash.py"""
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

# the build ID inside a core dump: note name (20 bytes, padded) + u32 version + ELF SHA as text
blob = b'junk' + b'ESP_CORE_DUMP_INFO\0\0' + struct.pack('<I', 2) + b'abcdef012\0' + b'more'
assert airflash.coredump_elf_sha(blob) == 'abcdef012' and airflash.coredump_elf_sha(b'nothing') == ''

# resume: a fake board answers 3 connections the way the component does (reply line, then log bytes)
import socket, threading  # noqa: E402
srv = socket.create_server(('127.0.0.1', 0))
hellos = []
scripted = [b'airflash 0000abcd 00000000\nline1\n',          # first connection: log 0xabcd from offset 0
            b'airflash 0000abcd 0000000000000006\nline2\n',  # resumed exactly where we stopped (6 bytes)
            b'airflash 00001234 0\nboot2\n']                   # the board's buffer started over (new id)


def fake_board():
    for reply in scripted:
        conn, _ = srv.accept()
        hellos.append(conn.recv(64).decode())
        conn.sendall(reply)
        conn.close()


threading.Thread(target=fake_board, daemon=True).start()
stream = airflash.board_stream('127.0.0.1', srv.getsockname()[1], quiet=1, retry=0.01)
got = b''
while b'boot2' not in got:
    got += next(stream)
assert hellos == ['airflash 00000000 0\n', 'airflash 0000abcd 6\n', 'airflash 0000abcd c\n'], hellos
assert got.count(b'line1') == 1 and got.count(b'line2') == 1
assert got.index(b'started over') < got.index(b'boot2')

print('ok')
