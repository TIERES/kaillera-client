"""Procura o placar na RAM do PS1 reproduzindo um .krec com o krec_harness.

Passo 1 (scan): a cada --every frames lê a RAM e, por byte, conta mudanças,
mudanças "estranhas" (nem +1 nem volta a 0) e o maior valor. Ao fim, guarda os
candidatos (só +1 ou volta a 0, poucas mudanças, valor pequeno) e a série deles.
Também confere os digests [SYNC] dos jogadores (a reprodução é fiel?).
Passo 2 (shots): salva PNGs da tela nos frames pedidos.
"""
import argparse
import ctypes as C
import json
import os
import shutil
import sys
import time

import numpy as np
from PIL import Image

HARNESS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "krec_harness")
sys.path.insert(0, HARNESS)
import krec_harness as KH  # noqa: E402
from krec import Krec  # noqa: E402


class ProbeCore(KH.Core):
    pixfmt = 2
    shot_request = None
    shot = None

    def _env(self, cmd, data):
        if (cmd & 0xFFFF) == 10 and data:  # SET_PIXEL_FORMAT
            self.pixfmt = C.cast(data, C.POINTER(C.c_int))[0]
            return True
        return super()._env(cmd, data)

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self._vcb = KH.VIDEO_T(self._video)
        self.lib.retro_set_video_refresh(self._vcb)

    def _video(self, data, w, h, pitch):
        if self.shot_request is not None and data:
            self.shot = (C.string_at(data, h * pitch), w, h, pitch, self.pixfmt)
            self.shot_request = None

    swap = False

    def set_input(self, rec):
        super().set_input(rec)
        if self.swap:  # --swap: o controle 1 recebe o input do jogador 2 e vice-versa
            self.joy[0], self.joy[1] = self.joy[1], self.joy[0]


def save_png(shot, path, scale_to=4096):
    raw, w, h, pitch, fmt = shot
    if fmt == 2:  # RGB565
        a = np.frombuffer(raw, dtype="<u2").reshape(h, pitch // 2)[:, :w]
        r = ((a >> 11) & 0x1F) << 3
        g = ((a >> 5) & 0x3F) << 2
        b = (a & 0x1F) << 3
        img = np.dstack([r, g, b]).astype(np.uint8)
    else:  # XRGB8888
        a = np.frombuffer(raw, dtype="<u4").reshape(h, pitch // 4)[:, :w]
        img = np.dstack([(a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF]).astype(np.uint8)
    im = Image.fromarray(img)
    if w > scale_to:
        im = im.resize((scale_to, int(h * scale_to / w)))
    im.save(path)


def content_id(game):
    """(crc32, tamanho) do arquivo do jogo - o "content" do fingerprint do fork
    (ksync_hash_content) e o content_id do M. Card Online, para um .bin/.iso."""
    import zlib
    crc, size = 0, 0
    with open(game, "rb") as f:
        while chunk := f.read(1 << 20):
            crc = zlib.crc32(chunk, crc)
            size += len(chunk)
    return crc & 0xFFFFFFFF, size


def open_core(args, k):
    KH.Core = ProbeCore
    opts = list(args.opt)
    # Cartões do M. Card Online: sempre a cópia original (o jogo grava no
    # cartão durante a partida), no harness_saves desta saída.
    saves = os.path.join(args.out, "harness_saves")
    os.makedirs(saves, exist_ok=True)
    for slot, card in ((1, args.card1), (2, args.card2)):
        target = os.path.join(saves, "pcsx-card%d.mcd" % slot)
        if os.path.exists(target):
            os.remove(target)
        if card:
            shutil.copyfile(card, target)
            opts.append("pcsx_rearmed_memcard%d=shared" % slot)
    ns = argparse.Namespace(opt=opts, ra=args.ra, game=args.game, verbose=False, workdir=args.out,
                            core=None, av=3)
    core = KH.open_core(ns, k)
    core.swap = args.swap
    return core


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["scan", "shots", "series", "record", "snap"])
    ap.add_argument("krec")
    ap.add_argument("--game", required=True)
    ap.add_argument("--ra", default=r"D:\JOGOS\RetroArch-1.16.0.FFW.TIERES.0.5")
    ap.add_argument("--out", required=True)
    ap.add_argument("--every", type=int, default=30)
    ap.add_argument("--frames", type=int, default=0)
    ap.add_argument("--at", default="", help="shots: frames separados por virgula, ou a:b:passo")
    ap.add_argument("--addr", default="", help="series: enderecos hex separados por virgula")
    ap.add_argument("--opt", action="append", default=[])
    ap.add_argument("--card1", help="cartão do slot 1 (.mcd do M. Card Online)")
    ap.add_argument("--card2", help="cartão do slot 2")
    ap.add_argument("--swap", action="store_true", help="troca os inputs dos jogadores 1 e 2")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    k = Krec(args.krec)
    core = open_core(args, k)
    last = min(len(k.frames), args.frames) if args.frames else len(k.frames)
    recorded = k.digests()
    dg = KH.Digester(core)
    first_bad = None
    ok = 0
    t0 = time.time()

    if args.mode == "shots":
        wanted = []
        for part in args.at.split(","):
            if ":" in part:
                a, b, s = (int(x) for x in part.split(":"))
                wanted += list(range(a, b + 1, s))
            elif part:
                wanted.append(int(part))
        wanted = sorted(set(wanted))
        wi = 0
        for f in range(1, last + 1):
            core.set_input(k.frames[f - 1])
            if wi < len(wanted) and f >= wanted[wi]:
                core.shot_request = f
            core.run()
            if core.shot is not None:
                save_png(core.shot, os.path.join(args.out, "f%06d.png" % f))
                core.shot = None
                wi += 1
            if wi >= len(wanted):
                break
        print("shots ok", wi)
        return

    if args.mode == "snap":
        # RAM inteira nos frames de --at (ram_<frame>.bin).
        wanted = sorted(int(x) for x in args.at.split(","))
        for f in range(1, wanted[-1] + 1):
            core.set_input(k.frames[f - 1])
            core.run()
            if f in wanted:
                open(os.path.join(args.out, "ram_%06d.bin" % f), "wb").write(core.ram())
                open(os.path.join(args.out, "scr_%06d.bin" % f), "wb").write(core.scratch())
        print("snap ok", len(wanted))
        return

    if args.mode == "record":
        # Série dos candidatos do scan (cand.npy) a cada --every frames.
        cand = np.load(os.path.join(args.out, "cand.npy"))
        frames, rows = [], []
        for f in range(1, last + 1):
            core.set_input(k.frames[f - 1])
            core.run()
            if f % args.every == 0:
                rows.append(np.frombuffer(core.ram(), dtype=np.uint8)[cand].copy())
                frames.append(f)
        np.savez(os.path.join(args.out, "record.npz"), cand=cand, frames=np.array(frames), values=np.array(rows))
        print("record ok", len(frames), "amostras x", len(cand))
        return

    if args.mode == "series":
        addrs = [int(x, 16) & 0x1FFFFF for x in args.addr.split(",")]
        rows = []
        for f in range(1, last + 1):
            core.set_input(k.frames[f - 1])
            core.run()
            w = dg.after_frame(f)
            if w is not None and (0, w) in recorded:
                if dg.done[w] in set(recorded[(0, w)].values()):
                    ok += 1
                elif first_bad is None:
                    first_bad = (w, f)
                    print("DIVERGE na janela %d (frame %d, %s)" % (w, f, KH.fmt_time(f)), flush=True)
            if f % args.every == 0:
                ram = np.frombuffer(core.ram(), dtype=np.uint8)
                rows.append([f] + [int(ram[a]) for a in addrs])
        print("janelas conferidas: %d ok; divergencia: %s" % (ok, first_bad))
        prev = None
        for r in rows:
            if prev is None or r[1:] != prev[1:]:
                print(KH.fmt_time(r[0]), r[0], r[1:])
            prev = r
        return

    size = core.ram_size
    prev = None
    changes = np.zeros(size, np.uint32)
    weird = np.zeros(size, np.uint32)
    vmax = np.zeros(size, np.uint8)
    samples = []
    for f in range(1, last + 1):
        core.set_input(k.frames[f - 1])
        core.run()
        w = dg.after_frame(f)
        if w is not None and (0, w) in recorded:
            mine = dg.done[w]
            theirs = set(recorded[(0, w)].values())
            if mine in theirs:
                ok += 1
            elif first_bad is None:
                first_bad = (w, f)
                print("DIVERGE na janela %d (frame %d, %s)" % (w, f, KH.fmt_time(f)), flush=True)
        if f % args.every == 0:
            ram = np.frombuffer(core.ram(), dtype=np.uint8).copy()
            if prev is not None:
                diff = ram != prev
                changes += diff
                weird += diff & (ram != (prev + 1).astype(np.uint8)) & (ram != 0)
            np.maximum(vmax, ram, out=vmax)
            prev = ram
            samples.append(f)
        if f % 20000 == 0:
            print("  frame %d (%s) %.0f fps" % (f, KH.fmt_time(f), f / (time.time() - t0)), flush=True)
    cand = np.nonzero((weird == 0) & (changes >= 1) & (changes <= 40) & (vmax <= 20))[0]
    print("janelas conferidas: %d ok; divergencia: %s" % (ok, first_bad))
    print("candidatos: %d" % len(cand))
    np.save(os.path.join(args.out, "cand.npy"), cand)
    json.dump({"ok": ok, "first_bad": first_bad, "frames": last, "every": args.every},
              open(os.path.join(args.out, "scan.json"), "w"))


if __name__ == "__main__":
    main()
