"""Jogador robô dos Winning Eleven no core do RetroArch TIERES - sem replay.

Liga o pcsx_rearmed com as mesmas opções que o fork força (krec_harness.Core),
aperta botões nos controles 1 e 2, salva/carrega estados (para tentar de novo
um lance até ele sair), lê RAM/scratchpad, tira prints e grava TUDO o que foi
apertado como .krec - o mesmo formato dos replays, para reproduzir depois no
c_live_test.py (código C do fork), no live_sim.py e no score_probe.py.

IMPORTANTE sobre os .krec: o arquivo só reproduz a partida se as entradas forem
as que o core realmente rodou desde o boot. Ao carregar um estado (load), o
registro volta junto (as entradas gravadas depois do save são descartadas).

Uso típico:
    from webot import Bot, BTN
    bot = Bot(r"...\\jogo.chd", r"...\\saida")
    bot.press(BTN.START, frames=4, after=60)      # controle 1
    bot.hold(0, 120)                               # nada por 2 s
    s = bot.save()                                 # ponto para tentar de novo
    bot.load(s)
    bot.shot("menu.png")
    ram = bot.ram(); scr = bot.scratch()
    bot.write_krec("partida.krec")
"""
import os
import struct
import sys
import time

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(TOOLS, "score_probe"))
sys.path.insert(0, os.path.join(TOOLS, "krec_harness"))
import krec_harness as KH  # noqa: E402
import score_probe as SP  # noqa: E402

# (Os scripts de cenário de 2026-11-08 - WE3/WE4 - usam este módulo: gols, gol
# contra, pênalti, prorrogação, gol de ouro, pênaltis, saída pela pausa e DEMO.)

# RetroArch do pacote 0.5 desta máquina: o mesmo core (md5 94f3684f) e .opt dos jogadores.
RA = r"D:\JOGOS\RetroArch-1.16.0.FFW.TIERES.0.5"


class BTN:
    """Bits do RETRO_DEVICE_JOYPAD (pcsx_rearmed: Cross=B, Circle=A, Square=Y, Triangle=X)."""
    CROSS = 1 << 0
    SQUARE = 1 << 1
    SELECT = 1 << 2
    START = 1 << 3
    UP = 1 << 4
    DOWN = 1 << 5
    LEFT = 1 << 6
    RIGHT = 1 << 7
    CIRCLE = 1 << 8
    TRIANGLE = 1 << 9
    L1 = 1 << 10
    R1 = 1 << 11
    L2 = 1 << 12
    R2 = 1 << 13


class Bot:
    def __init__(self, game, out, opts=None, players=2):
        os.makedirs(out, exist_ok=True)
        self.game = game
        self.out = out
        self.players = players
        KH.Core = SP.ProbeCore
        self.core = SP.ProbeCore(RA, game, dict(opts or {}), False, out, None)
        self.core.av = 3
        self.frame = 0
        self.inputs = []  # o .krec: um payload por frame (12 bytes por jogador)

    # --- entradas -----------------------------------------------------------
    def step(self, p1=0, p2=0, n=1):
        """Roda n frames com os botões p1 (controle 1) e p2 (controle 2)."""
        rec = b"".join(struct.pack("<6h", 0, m, 0, 0, 0, 0) for m in (p1, p2)[:self.players])
        rec += b"\0" * (12 * self.players - len(rec))
        for _ in range(n):
            self.core.set_input(rec)
            self.core.run()
            self.inputs.append(rec)
            self.frame += 1

    def hold(self, mask=0, frames=1, p2=0):
        self.step(mask, p2, frames)

    def press(self, mask, frames=4, after=20, pad=1):
        """Aperta e solta (frames apertado, depois `after` frames sem nada)."""
        if pad == 1:
            self.step(mask, 0, frames)
        else:
            self.step(0, mask, frames)
        self.step(0, 0, after)

    # --- estados ------------------------------------------------------------
    def save(self):
        return (self.core.serialize(), self.frame, len(self.inputs))

    def load(self, s):
        blob, frame, n = s
        self.core.unserialize(blob)
        self.frame = frame
        del self.inputs[n:]

    # --- leitura ------------------------------------------------------------
    def ram(self):
        return self.core.ram()

    def scratch(self):
        return self.core.scratch()

    def u8(self, addr):
        """Byte da RAM (endereço 0x80xxxxxx ou só o deslocamento) ou do scratchpad (0x1F800xxx)."""
        if addr >= 0x1F800000 and addr < 0x1F800400:
            return self.scratch()[addr - 0x1F800000]
        return self.ram()[addr & 0x1FFFFF]

    def u16(self, addr):
        if addr >= 0x1F800000 and addr < 0x1F800400:
            s = self.scratch()
            o = addr - 0x1F800000
            return s[o] | (s[o + 1] << 8)
        r = self.ram()
        o = addr & 0x1FFFFF
        return r[o] | (r[o + 1] << 8)

    def shot(self, name):
        """PNG da próxima imagem (roda 1 frame sem botões para pegá-la)."""
        self.core.shot_request = True
        self.step(0, 0, 1)
        path = os.path.join(self.out, name)
        if self.core.shot:
            SP.save_png(self.core.shot, path)
        return path

    # --- gravação -----------------------------------------------------------
    def write_krec(self, path, app="webot", nicks=("BOT1", "BOT2")):
        """Grava as entradas como .krec (KRC0) - o krec.py/krec_harness leem."""
        head = b"KRC0" + app.encode()[:127].ljust(128, b"\0") + os.path.basename(self.game).encode(
            "latin-1", "replace")[:127].ljust(128, b"\0")
        head += struct.pack("<iii", int(time.time()), 1, self.players)
        assert len(head) == 272
        body = bytearray()
        for rec in self.inputs:
            body += b"\x12" + struct.pack("<h", len(rec)) + rec
        with open(path, "wb") as f:
            f.write(head + bytes(body))
        return path
