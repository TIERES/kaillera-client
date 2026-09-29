"""Fake EmuLinker-K login for kmsg_client.exe: HELLO, 4 ServerACK round trips, then the
12-bundle post-login burst exactly as EmuLinker sends it (ServerStatus, 9 InfoMsgs, the
user's own UserJoined; each bundle carries the newest message plus the previous 4).

--mode picks what the "network" does to that burst:
  clean    all 12 bundles, in order
  drop5    the first 5 bundles never arrive - the only ones carrying ServerStatus
           (what Tassio's ISP does to the Oracle servers' sub-millisecond burst)
  reorder  bundles 6..12 arrive before 1..5
  drop10   the first 10 bundles never arrive (a jump of 10+ serials)

Serves one login, then exits. Usage: python fake_elk.py --port 27999 --mode drop5"""
import argparse, socket, struct

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=27999)
ap.add_argument("--mode", default="clean", choices=["clean", "drop5", "reorder", "drop10"])
args = ap.parse_args()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", args.port))
s.settimeout(15)

sent = []  # message bodies (type byte + data), index = serial


def bundle(newest):
    msgs = [(n, sent[n]) for n in range(newest, max(-1, newest - 5), -1)]
    return bytes([len(msgs)]) + b"".join(struct.pack("<HH", n, len(b)) + b for n, b in msgs)


def queue(body):
    sent.append(body)
    return bundle(len(sent) - 1)


def parse(pkt):
    out = []
    p = 1
    for _ in range(pkt[0]):
        n, ln = struct.unpack_from("<HH", pkt, p)
        out.append((n, pkt[p + 4:p + 4 + ln]))
        p += 4 + ln
    return out


data, hello_from = s.recvfrom(4096)
assert data.startswith(b"HELLO0.83"), data
s.sendto(b"HELLOD00D%d\x00" % args.port, hello_from)

client = None
user = b"?"
seen = set()
acks = 0
server_ack = b"\x05\x00" + struct.pack("<IIII", 0, 1, 2, 3)
while True:
    data, frm = s.recvfrom(4096)
    client = frm
    for n, body in parse(data)[::-1]:
        if n in seen:
            continue
        seen.add(n)
        if body[0] == 0x03:  # UserInformation: user\0 app\0 conn
            user = body[1:].split(b"\x00")[0]
            s.sendto(queue(server_ack), client)
        elif body[0] == 0x06:  # ClientACK
            acks += 1
            if acks < 4:
                s.sendto(queue(server_ack), client)
    if acks >= 4:
        break

info = [b"Bem-vindo ao One Two Server Teste!", b".", b"[ W I N N I N G   E L E V E N   2 0 0 2 ]",
        b">>>> CAMPEONATOS E ISOS <<<<", b"https://we2002.wgs.dev.br",
        b"Acompanhe os campeonatos em vigor das principais ligas de Winning Eleven 2002.",
        b"Faca o download do emulador e tambem da ISO de cada campeonato. PARTICIPE!", b".", b".",
        b"EmuLinker-K v1.0.2 (fake_elk)"]
burst = [queue(b"\x04\x00" + struct.pack("<II", 0, 0))]  # ServerStatus: 0 users, 0 games
burst += [queue(b"\x17server\x00" + line + b"\x00") for line in info]
burst.append(queue(b"\x02" + user + b"\x00" + struct.pack("<HIB", 7, 50, 1)))  # own UserJoined

first = len(sent) - len(burst)
order = list(range(len(burst)))
if args.mode == "drop5":
    order = order[5:]
elif args.mode == "drop10":
    order = order[10:]
elif args.mode == "reorder":
    order = order[5:] + order[:5]
for i in order:
    s.sendto(burst[i], client)
print(f"fake_elk: sent burst serials {first}..{len(sent) - 1} to {client}, mode={args.mode}", flush=True)

# Report what the client sends afterwards (a CACK here means it processed a stale ServerACK).
s.settimeout(3)
try:
    while True:
        data, frm = s.recvfrom(4096)
        for n, body in parse(data)[::-1]:
            if n not in seen:
                seen.add(n)
                print(f"fake_elk: client sent serial {n} type {body[0]:#04x}", flush=True)
except (socket.timeout, ConnectionResetError):
    pass  # Windows reports the client's closed port as a reset
