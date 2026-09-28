# .krec parser (KRC0/KRC1) - same record format kailleraclient.cpp writes
# locally and n02_stream.cpp streams: 0x12 input frame, 0x08 chat, 0x14 drop.
import re
import struct
import sys

HEADER_KRC1 = 400
HEADER_KRC0 = 272

# "[SYNC] v=1 n=<nonce> e=<epoch> d=<window>:<16 hex>" (e= missing before the
# rollback feature = epoch 0) - see retroarch-k3's kaillera/kaillera_sync.c.
SYNC_DIGEST_RE = re.compile(r"\[SYNC\] .*?n=(\S+)(?: e=(\d+))? d=(\d+):([0-9A-F]{16})")
MEMCARD_MARKER_NICK = "n02"


class Krec:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.path = path
        self.raw = d
        self.magic = d[:4]
        if self.magic not in (b"KRC0", b"KRC1"):
            raise ValueError("%s: not a .krec (magic %r)" % (path, self.magic))
        self.app = d[4:132].split(b"\0")[0].decode("latin-1")
        self.game = d[132:260].split(b"\0")[0].decode("latin-1")
        self.playerno, self.numplayers = struct.unpack_from("<ii", d, 264)
        self.header_size = HEADER_KRC1 if self.magic == b"KRC1" else HEADER_KRC0
        self.names = []
        if self.magic == b"KRC1":
            self.names = [d[272 + i * 32:272 + (i + 1) * 32].split(b"\0")[0].decode("latin-1") for i in range(4)]
        self.frames = []      # input payloads (bytes, may be empty), index = frame record index
        self.frame_pos = []   # byte offset of each frame record (file/stream coordinates)
        self.chats = []       # (frames_before, nick, msg, byte_offset)
        self.drops = []       # (frames_before, nick, playerno)
        self.truncated = None  # byte offset where parsing stopped early, if it did
        p = self.header_size
        while p < len(d):
            start = p
            t = d[p]
            p += 1
            try:
                if t == 0x12:
                    (l,) = struct.unpack_from("<h", d, p)
                    p += 2
                    if l < 0 or p + l > len(d):
                        raise ValueError
                    self.frame_pos.append(start)
                    self.frames.append(d[p:p + l])
                    p += l
                elif t == 0x08:
                    e = d.index(b"\0", p)
                    nick = d[p:e].decode("latin-1")
                    p = e + 1
                    e = d.index(b"\0", p)
                    msg = d[p:e].decode("latin-1")
                    p = e + 1
                    self.chats.append((len(self.frames), nick, msg, start))
                elif t == 0x14:
                    e = d.index(b"\0", p)
                    nick = d[p:e].decode("latin-1")
                    p = e + 1
                    (pn,) = struct.unpack_from("<i", d, p)
                    p += 4
                    self.drops.append((len(self.frames), nick, pn))
                else:
                    raise ValueError
            except (ValueError, struct.error):
                self.truncated = start
                break

    def memcard_marker(self):
        """True = "Sem M. Card" (no card), False = with card, None = no marker."""
        for _, nick, msg, _ in self.chats[:4]:
            if nick == MEMCARD_MARKER_NICK and msg.startswith("[Sem M. Card]"):
                return "desativado" not in msg
        return None

    def digests(self):
        """{(epoch, window): {nick: hash}} from the players' in-band [SYNC] d= lines."""
        out = {}
        for fr, nick, msg, _ in self.chats:
            m = SYNC_DIGEST_RE.search(msg)
            if m:
                epoch = int(m.group(2) or 0)
                out.setdefault((epoch, int(m.group(3))), {})[nick] = m.group(4)
        return out

    def first_rollback_frame(self):
        for fr, nick, msg, _ in self.chats:
            m = SYNC_DIGEST_RE.search(msg)
            if m and int(m.group(2) or 0) > 0:
                return fr
        return None

    def command_frames(self):
        """Frame record indexes carrying a netplay command byte (swap/save/load/reset/rollback...)."""
        out = []
        for i, f in enumerate(self.frames):
            for p in range(len(f) // 12):
                if f[p * 12:p * 12 + 2] != b"\0\0":
                    out.append((i, p, struct.unpack_from("<H", f, p * 12)[0]))
        return out

    def frames_before_offset(self, offset):
        """Number of frame records that start before `offset`, and whether
        `offset` is exactly a record boundary."""
        import bisect
        k = bisect.bisect_left(self.frame_pos, offset)
        boundaries = set(self.frame_pos) | {c[3] for c in self.chats}
        if self.frame_pos:
            last = self.frame_pos[-1]
            boundaries.add(last + 3 + len(self.frames[-1]))
        return k, (offset in boundaries)


def summary(path):
    k = Krec(path)
    from collections import Counter
    print("%s: %s | %s | %s | %d jogadores %s" % (path, k.magic.decode(), k.app, k.game, k.numplayers, [n for n in k.names if n]))
    print("  frames %d (%s) - chats %d - drops %s" % (len(k.frames), dict(Counter(len(f) for f in k.frames).most_common(4)), len(k.chats), k.drops))
    mc = k.memcard_marker()
    print("  marca de memory card:", {True: "Sem M. Card", False: "COM memory card", None: "(sem marca)"}[mc])
    dg = k.digests()
    print("  digests [SYNC] d=: %d janelas" % len(dg))
    if k.truncated is not None:
        print("  ATENCAO: registro invalido/truncado no byte %d" % k.truncated)


if __name__ == "__main__":
    for p in sys.argv[1:]:
        summary(p)
