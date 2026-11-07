"""Simula o placar ao vivo de uma partida: reproduz um .krec no krec_harness
e manda para o WE Camp (POST /api/live/match) o mesmo estado que o RetroArch
TIERES manda - a lógica de MatchWatch abaixo é a do fork (retroarch-k3,
kaillera/kaillera_sync.c, ksync_live_*); mantenha as duas iguais.

    live_sim.py <partida.krec> --game <iso> [--card1 x.mcd] [--url https://...]
                [--speed 4] [--both] [--dry]

--speed: 1 = tempo real, 4 = quatro vezes mais rápido (0 = sem esperar).
--both: manda como os dois PCs (o WE Camp junta e confere o placar no fim).
--dry: só imprime o que mandaria.
As simulações vão com test=1: só o admin do site vê os cards.
"""
import argparse
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import score_probe as SP  # noqa: E402
from krec import Krec  # noqa: E402

DEFAULT_KEY = "25dc982372ced3c0f18a53e18d9224a8ebbec36974e9f6520ea8fb3e88b0aa22"  # N02_STREAM_DEFAULT_API_KEY

# Mapa da RAM (README.md) - endereços físicos (sem o 0x80000000).
TEAM_ID = 0x75564
PERIOD = 0x75566
CLOCK_LEFT = 0x7591C
SCORE = 0x75702
GOALS = (0x755C4, 0x75644)
ROSTER = (0x64570, 0x646E0)
PADS = 0x758D9
NAME_PTRS = 0x15DBC4

SAMPLE_FRAMES = 30       # lê a RAM a cada meio segundo
HEARTBEAT_FRAMES = 900   # e manda pelo menos a cada 15 s (o relógio do card)
MIN_KICKOFF_CLOCK = 1800  # um tempo de pelo menos 30 s (partidas de 2 minutos para cima)


def cstr(ram, addr, cap):
    raw = ram[addr:addr + cap]
    end = raw.find(b"\0")
    raw = raw[:end if end >= 0 else cap]
    return raw.decode("latin-1") if raw and all(32 <= b < 127 for b in raw) else ""


def team_name(ram, team_id):
    (ptr,) = struct.unpack_from("<I", ram, NAME_PTRS + 4 * team_id)
    if not 0x80000000 <= ptr < 0x80000000 + len(ram):
        return ""
    name = cstr(ram, ptr & 0x1FFFFF, 24)
    return name if 2 <= len(name) <= 16 and name.upper() == name else ""


def read_state(ram):
    """Estado da partida na RAM, ou None se não parece uma partida do motor do WE 2002."""
    period, clock = ram[PERIOD], struct.unpack_from("<H", ram, CLOCK_LEFT)[0]
    pads = list(ram[PADS:PADS + 4])
    if period > 4 or any(p not in (0, 1, 0xFF) for p in pads):
        return None
    teams = []
    for side in (0, 1):
        team_id, goals = ram[TEAM_ID + side], ram[SCORE + side]
        if team_id >= 128 or goals > 30:
            return None
        scorers = []
        for n in range(32):
            scorer, assist, minute, _ = ram[GOALS[side] + 4 * n:GOALS[side] + 4 * n + 4]
            if scorer == 0xFF:
                break
            if scorer >= 23 or minute > 200:
                return None
            scorers.append([cstr(ram, ROSTER[side] + 16 * scorer, 16), minute,
                            cstr(ram, ROSTER[side] + 16 * assist, 16) if assist < 23 else ""])
        teams.append({"id": team_id, "name": team_name(ram, team_id), "goals": goals, "scorers": scorers})
    return {"period": period, "clock": clock, "pads": pads, "teams": teams}


class MatchWatch:
    """Pontapé inicial, gols, troca de tempo e fim - o que o fork faz por frame."""

    def __init__(self, content):
        self.content = content
        self.active = False
        self.kickoff = 0
        self.half = 18000
        self.last = None        # último estado lido
        self.sent_key = None    # o que mudou desde o último envio
        self.sent_frame = 0

    def sample(self, frame, ram):
        """Chamado a cada SAMPLE_FRAMES; devolve o estado a mandar, ou None."""
        st = read_state(ram)
        prev, self.last = self.last, st
        if st is None:
            self.active = False
            return None
        final = False
        if prev is not None:
            # Pontapé inicial do 1º tempo: o relógio salta de 0 para a duração do tempo.
            if st["period"] == 0 and st["clock"] >= MIN_KICKOFF_CLOCK and prev["clock"] == 0:
                self.active, self.kickoff, self.half = True, frame, st["clock"]
            # Entrou no meio (retry-connect, ou o RetroArch abriu já em jogo): relógio correndo.
            elif not self.active and 0 < st["clock"] < prev["clock"]:
                self.active, self.kickoff = True, 0
            # Fim: depois do 2º tempo, o período volta a 0 com o relógio zerado.
            elif self.active and prev["period"] == 1 and st["period"] == 0 and st["clock"] == 0:
                final = True
        if not self.active:
            return None
        key = json.dumps([st["period"], st["pads"], st["teams"]])
        if not final and key == self.sent_key and frame - self.sent_frame < HEARTBEAT_FRAMES:
            return None
        self.sent_key, self.sent_frame = key, frame
        if final:
            self.active = False
            st = prev  # o placar do fim (o período já voltou a 0)
        return {"v": 1, "content": self.content, "kickoff": self.kickoff, "frame": frame, "half": self.half,
                "final": int(final), **st}


def post(url, key, state, players, reporter, room):
    body = urllib.parse.urlencode({"state": json.dumps(state), "players": ",".join(players), "reporter": reporter,
                                   "room": room, "test": "1"}).encode()
    req = urllib.request.Request(url.rstrip("/") + "/api/live/match", data=body, headers={"X-Api-Key": key})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.read().decode().strip().replace("\n", " ")
    except urllib.error.HTTPError as e:
        return "HTTP %d %s" % (e.code, e.read()[:200])
    except OSError as e:
        return "erro: %s" % e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("krec")
    ap.add_argument("--game", required=True)
    ap.add_argument("--ra", default=r"D:\JOGOS\RetroArch-1.16.0.FFW.TIERES.0.5")
    ap.add_argument("--card1")
    ap.add_argument("--card2")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TEMP", "."), "live_sim"))
    ap.add_argument("--url", default="http://localhost:5000")
    ap.add_argument("--key", default=DEFAULT_KEY)
    ap.add_argument("--speed", type=float, default=4)
    ap.add_argument("--both", action="store_true")
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--frames", type=int, default=0)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    k = Krec(a.krec)
    ns = argparse.Namespace(opt=[], ra=a.ra, game=a.game, out=a.out, card1=a.card1, card2=a.card2, swap=False)
    core = SP.open_core(ns, k)
    content = "%08X:%X" % SP.content_id(a.game)
    players = [n for n in k.names[:k.numplayers] if n]
    room = k.game
    watch = MatchWatch(content)
    last = min(len(k.frames), a.frames) if a.frames else len(k.frames)
    t0, paced_from = time.time(), None
    print("%s | %s | %s | jogadores %s" % (os.path.basename(a.krec), room, content, players), flush=True)
    for f in range(1, last + 1):
        core.set_input(k.frames[f - 1])
        core.run()
        if f % SAMPLE_FRAMES:
            continue
        st = watch.sample(f, core.ram())
        if watch.active and paced_from is None:
            paced_from = (f, time.time())  # do pontapé inicial em diante, no ritmo pedido
        if paced_from and a.speed > 0:
            ahead = (f - paced_from[0]) / 60.0 / a.speed - (time.time() - paced_from[1])
            if ahead > 0:
                time.sleep(ahead)
        if st is None:
            continue
        t1, t2 = st["teams"]
        line = "%6d %s per %d relogio %5d  %s %d x %d %s%s" % (
            f, SP.KH.fmt_time(f), st["period"], st["clock"], t1["name"] or t1["id"], t1["goals"], t2["goals"],
            t2["name"] or t2["id"], "  FIM" if st["final"] else "")
        if a.dry:
            print(line, flush=True)
            continue
        replies = [post(a.url, a.key, st, players, r, room) for r in (players if a.both else players[:1])]
        print(line, " -> ", " | ".join(replies), flush=True)
    print("fim em %.0f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
