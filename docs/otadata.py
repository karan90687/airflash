import struct, sys, binascii
STATES = {0:"NEW",1:"PENDING_VERIFY",2:"VALID",3:"INVALID",4:"ABORTED",0xFFFFFFFF:"UNDEFINED"}
d = open(sys.argv[1], "rb").read()
for i, off in enumerate((0, 0x1000)):
    seq, label, state, crc = struct.unpack_from("<I20sII", d, off)
    if seq == 0xFFFFFFFF: print(f"sector {i}: blank"); continue
    ok = crc == binascii.crc32(struct.pack("<I", seq), 0xFFFFFFFF)
    print(f"sector {i}: seq={seq} -> ota_{(seq-1)%2}  state={STATES.get(state, hex(state))}  crc {'ok' if ok else 'BAD'}")
