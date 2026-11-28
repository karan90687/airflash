"""Write the updater's upd_cfg partition image (contract C-UPDCFG) and print the pairing token.

    python upd_cfg.py <out.bin> <partition size> <build/config/sdkconfig.json>

Image: "AFCFG001" | token, ASCII, NUL-padded to 64 bytes | CRC32 (zlib, little-endian) of the 72 bytes before
| 0xFF up to the partition size.
Token: menuconfig's UPD_TOKEN, else $AIRFLASH_TOKEN, else ~/.airflash/token, else a new random one.
~/.airflash/token is created (0600) with the token when it doesn't exist; an existing one is never overwritten.
"""
import json
import os
import re
import secrets
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b'AFCFG001'
TOKEN_RE = re.compile(r'[\x21-\x7e]{1,63}')  # printable ASCII without spaces; generated ones are 32 hex
MENU = 'menuconfig (airflash > Pairing token)'


def image(token, size):
    head = MAGIC + token.encode().ljust(64, b'\0')
    head += struct.pack('<I', zlib.crc32(head))
    return head + b'\xff' * (size - len(head))


def check(token, what, fix=''):
    if not TOKEN_RE.fullmatch(token):
        sys.exit(f'upd_cfg: {what} must be 1-63 printable ASCII characters without spaces{fix}')
    return token


def get_token(menu, path):
    """(token, source); source None = the file at path, or a new token for it. Exits on a malformed token."""
    if menu:
        return check(menu, f'the token in {MENU}', '; empty = generate one'), MENU
    env = os.environ.get('AIRFLASH_TOKEN')
    if env:  # empty = unset
        return check(env, 'AIRFLASH_TOKEN'), 'the AIRFLASH_TOKEN environment variable'
    if path.exists():
        return check(path.read_text().strip(), str(path), '; fix it or delete it to get a new one'), None
    # Random, not timestamp + MAC as first planned: the MAC is broadcast on Wi-Fi and timestamps are guessable.
    return secrets.token_hex(16), None


def save(path, token):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)  # 0600 where the OS supports it
    with os.fdopen(fd, 'w') as f:
        f.write(token + '\n')


def main():
    out, size, cfg = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
    path = Path.home() / '.airflash' / 'token'
    token, src = get_token(json.loads(Path(cfg).read_text()).get('UPD_TOKEN', ''), path)
    old = path.read_text().strip() if path.exists() else None
    if old is None:
        save(path, token)
    Path(out).write_bytes(image(token, size))
    via = f'from {src}, ' if src else ''
    line2 = (f'{via}saved in {path}; the airflash tool on this laptop uses it automatically.' if old in (None, token)
             else f'from {src}; not saved, see the note below.')
    print(f'airflash token: {token}\n'
          f'  {line2}\n'
          f'  Anyone else who should flash or watch this board needs this token (file or AIRFLASH_TOKEN).')
    if old not in (None, token):
        print("note: this laptop's airflash tool uses a different token (~/.airflash/token): "
              "set AIRFLASH_TOKEN to this one, or replace that file if all your boards use it")
    if len(token) < 16:
        print('note: short token: someone on your Wi-Fi who records one login could guess it')


if __name__ == '__main__':
    main()
