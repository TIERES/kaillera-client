"""Monta o "card ao vivo" a partir de uma foto da RAM (ram_<frame>.bin do
score_probe.py snap): times, quem controla cada um, placar, tempo e gols.

Endereços do motor do Winning Eleven 2002 (conferidos em Copa Africana CAN
2025 e Euro vs Copa America 2004, ver README.md). Índice 0 = time LOCAL do
jogo, 1 = VISITA.

    card.py <ram.bin> [<ram.bin> ...]
"""
import struct
import sys

TEAM_ID = (0x75564, 0x75565)        # índice do time na tabela de nomes
PERIOD = 0x75566                    # 0 = 1º tempo, 1 = 2º (volta a 0 depois do apito final)
CLOCK_LEFT = 0x7591C                # u16: quadros que faltam no tempo (começa em duração*60)
SCORE = (0x75702, 0x75703)
GOALS = (0x755C4, 0x75644)          # 32 registros de 4 bytes: autor, assistência, minuto, 0 (FF = fim)
ROSTER = (0x64570, 0x646E0)         # 23 nomes de 16 bytes
PADS = 0x758D9                      # controles 1..4: 0 = LOCAL, 1 = VISITA, FF = sem time
NAME_PTRS = 0x15DBC4                # ponteiros para os nomes dos times (cada patch tem os seus)


def cstr(ram, addr):
    addr &= 0x1FFFFF
    return ram[addr:ram.index(b"\0", addr)].decode("latin-1")


def team_name(ram, team_id):
    (ptr,) = struct.unpack_from("<I", ram, NAME_PTRS + 4 * team_id)
    return cstr(ram, ptr) if 0x80000000 <= ptr < 0x80200000 else "?"


def player(ram, side, index):
    return cstr(ram, ROSTER[side] + 16 * index) if index < 23 else "?"


def card(ram, half_frames=None):
    left = struct.unpack_from("<H", ram, CLOCK_LEFT)[0]
    half_frames = half_frames or 18000  # 10 minutos de partida; o fork pega o 1º valor do tempo
    period = ram[PERIOD]
    minute = int(45 * (1 - left / half_frames)) + 45 * period
    out = {"periodo": period + 1, "minuto": minute if left else "%d+" % (45 * (period + 1)),
           "controles": {"%dP" % (i + 1): ("LOCAL", "VISITA")[v] for i, v in enumerate(ram[PADS:PADS + 4]) if v in (0, 1)},
           "times": []}
    for side in (0, 1):
        goals = []
        for n in range(32):
            scorer, assist, minute_, _ = ram[GOALS[side] + 4 * n:GOALS[side] + 4 * n + 4]
            if scorer == 0xFF:
                break
            goals.append("%s %d'%s" % (player(ram, side, scorer), minute_,
                                        "" if assist == 0xFF else " (assist. %s)" % player(ram, side, assist)))
        out["times"].append({"lado": ("LOCAL", "VISITA")[side], "id": ram[TEAM_ID[side]],
                             "nome": team_name(ram, ram[TEAM_ID[side]]), "gols": ram[SCORE[side]], "autores": goals})
    return out


if __name__ == "__main__":
    for path in sys.argv[1:]:
        c = card(open(path, "rb").read())
        a, b = c["times"]
        who = {side: [p for p, s in c["controles"].items() if s == side] for side in ("LOCAL", "VISITA")}
        print("== %s" % path)
        print("  %s (%s) %d x %d %s (%s)   %dº tempo, %s'" % (
            a["nome"], ", ".join(who["LOCAL"]) or "CPU", a["gols"], b["gols"], b["nome"],
            ", ".join(who["VISITA"]) or "CPU", c["periodo"], c["minuto"]))
        for t in (a, b):
            for g in t["autores"]:
                print("    %-8s %s" % (t["nome"], g))
