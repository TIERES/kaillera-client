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

# Mapa da RAM de cada motor (README.md) - endereços físicos (sem o 0x80000000).
# O jogo é reconhecido pelo nome do executável que o kernel guarda em SERIAL;
# os patches mantêm o do motor. Lado 0 = LOCAL do jogo.
SERIAL = 0x9E19
TEAMS = 128
# Campo no scratchpad (estado, fase, pênaltis... do WE3): o deslocamento lá com esta marca.
SCR = 0x40000000
BASE = {"scratch": False, "goal_size": 4, "roster_team": 0, "kick_on_run": False, "end_resets": False, "extra": 0,
        "phase": 0, "pens": 0, "kicker": 0, "touch": 0, "setpiece": 0, "state": 0,
        "pen_value": 2, "pen_state": None, "in_match": 0, "demo": 0, "demo_value": 0, "settings": 0,
        "stats": 0, "cards": None, "injuries": 0}
SET_EXTRA, SET_GOLDEN, SET_PENS = 1, 2, 4  # as opções da partida (bit 0 prorrogação, 1 gol de ouro, 2 pênaltis)
ENGINES = {
    "SLPM_870.56": dict(BASE, **{  # Winning Eleven 2002 (Euro vs Copa America, CAN 2025, BR Turbo...)
        "label": "WE2002", "sig": ((0x1701C, "070040122120c0022198400221900000"), (0x1D038, "b070000c200806240c00401401000324")),
        "team_id": 0x75564, "period": 0x75566, "clock": 0x7591C, "in_play": None,
        "score": 0x75702, "goals": (0x755C4, 0x75644), "roster": (0x64570, 0x646E0), "roster_len": 23,
        "name_len": 16, "pads": 0x758D9,
        # Só na tela de seleção de times: ponteiro para o nome de cada time e
        # o código de 3 letras (aqui, os próprios códigos, time 0 por último).
        "sel_names": 0xBF0E8, "sel_codes": 0x800DC, "code_ptrs": False, "mixed_case": False,
        # Fase (0/1 tempos, 2/3 prorrogação, 4 pênaltis), placar dos pênaltis
        # (LOCAL, VISITA) e o batedor (FF quando a disputa acabou).
        "phase": 0x7570E, "pens": 0x7570C, "kicker": 0x7571C,
        # Quem tocou por último na bola (posição em campo 1-22: 1-11 um lado,
        # 12-22 o outro, trocados a cada tempo; o índice no elenco é
        # (posição - 1) % 11) e a cobrança em andamento (2 = pênalti, até o
        # gol dela): o autor do gol contra e os gols de pênalti.
        "touch": 0x7B841, "setpiece": 0x7B499,
        # Estado da partida: 1 bola rolando, 7 gol, 11 comemoração, 0 saída de
        # bola, 10 o juiz encerrou o tempo (intervalo, fim, gol de ouro, pênaltis).
        "state": 0x75782,
        # A tela RESULT do fim: pares LOCAL/VISITA a partir daqui - chutes, impedimentos,
        # faltas, vermelhos (o 2º amarelo não conta), amarelos, escanteios (depois faltas
        # cobradas e pênaltis, que não vão); o cartão de cada jogador, um byte por índice
        # do elenco (1 amarelo, 2 o 2º amarelo - expulso -, 4 vermelho direto); e os
        # contundidos: registros {índice, time, 0, 0}, FF = o fim da lista.
        "stats": 0x756F2, "cards": (0x75568, 0x7557F), "injuries": 0x73968,
    }),
    "SLPM_862.91": dict(BASE, **{  # Winning Eleven 4 (Traducida)
        "label": "WE4", "sig": ((0x17034, "60000924200008241e00072421282701"), (0x1D03C, "0100843400200524002006240780013c")),
        "team_id": 0x750FB, "period": 0x750FD, "clock": 0x754A8, "in_play": 0x754B9,
        "score": 0x75293, "goals": (0x75157, 0x751D7), "roster": (0x7490C, 0x749E8), "roster_len": 22,
        "name_len": 10, "pads": 0x75469,
        "sel_names": 0xA67C0, "sel_codes": 0xA6AC0, "code_ptrs": True, "mixed_case": True,
        # Fase (como a do WE2002), placar dos pênaltis, último toque (1-22: 1-11 LOCAL nas
        # fases 0/2 e nos pênaltis, VISITA nas 1/3), a cobrança (3 = pênalti, com o estado
        # em 2), o estado e as opções da partida.
        "phase": 0x7529F, "pens": 0x7529D, "touch": 0x7B3DD, "setpiece": 0x75313, "state": 0x75312,
        "pen_value": 3, "pen_state": 2, "settings": 0x72FD2,
    }),
    "SLPM_861.62": dict(BASE, **{  # Winning Eleven 3 Final Version (e patches, "Camera torre TV mode")
        # team_id, period, clock, score e extra ficam no scratchpad (0x1F800000 + offset).
        "label": "WE3", "sig": ((0x1714C, "801f023c98024290801f013c9a0220a0"), (0x1D19C, "1c00b3af4c00b3931180073c78c1e724")),
        "scratch": True, "team_id": 0x2DD, "period": 0x2DF, "clock": 0x2C4, "in_play": None,
        "score": 0x2D1, "goals": (0x107FE5, 0x108045), "goal_size": 3,
        # Elencos por time (não por lado): roster[0] + time x 0xB0, 22 nomes de 8 bytes.
        "roster": (0xC8504, 0), "roster_team": 0xB0, "roster_len": 22, "name_len": 8,
        "pads": None,  # sem tabela: 1P no LOCAL, 2P na VISITA (a tela de seleção põe 1P à esquerda)
        "sel_names": 0x1D1B48, "sel_codes": 0x189B58, "code_ptrs": False, "mixed_case": True,
        # O relógio é armado na tela de formação e só corre do pontapé em diante;
        # no fim o placar zera junto com o período, e no menu depois do resultado
        # os times já podem mudar.
        "kick_on_run": True, "end_resets": True, "extra": 0x2C8,
        # O período é a própria fase (0/1 tempos, 2/3 prorrogação, 4 pênaltis); placar dos
        # pênaltis, último toque (1-22: 1-11 LOCAL nos períodos 0/2, VISITA nos 1/3), a
        # cobrança (3 = pênalti, com o estado em 2) e o estado - todos no scratchpad.
        "phase": SCR | 0x2DF, "pens": SCR | 0x2DB, "touch": SCR | 0x2AC, "setpiece": SCR | 0x29B,
        "state": SCR | 0x29A, "pen_value": 3, "pen_state": 2,
        # 1 dentro de uma partida (da formação até sair dela); o DEMO da tela de título
        # marca 1 em 0x10A2C2; as opções da partida em 0x10A307.
        "in_match": SCR | 0x198, "demo": 0x10A2C2, "demo_value": 1, "settings": 0x10A307,
    }),
}

SAMPLE_FRAMES = 30       # lê a RAM a cada meio segundo
PEN_FRAMES = 90          # gol logo depois do pênalti (WE2002: a cobrança vale "2" até o gol; WE3: só até o chute)
SHOOTOUT_IDLE = 1200     # disputa de pênaltis decidida, parada no valor de um erro: acabou
DRAW_FRAMES = 1500       # empate apitado: sem prorrogação/pênaltis em 25 s, terminou empatado
GRACE_FRAMES = 300       # 5 s depois do apito inicial antes de mandar (sair antes não conta)
LEFT_FRAMES = 60         # WE4: a bola parada sem o juiz encerrar o tempo = saíram pela pausa
STATE_OVER = 10
HEARTBEAT_FRAMES = 900   # e manda pelo menos a cada 15 s (o relógio do card)
MIN_KICKOFF_CLOCK = 1800  # um tempo de pelo menos 30 s (partidas de 2 minutos para cima)
OVER_FRAMES = 600        # WE3: 10 s sem tempo nenhum no 2º tempo = fim
NO_PADS = [0, 1, 0xFF, 0xFF]
EVENTS = 24              # cartões / contusões guardados por lado
INJURY_RECORDS = 10      # da lista de contundidos do WE2002: os que ele limpa ao carregar a partida
CARD_YELLOW, CARD_SECOND, CARD_RED = 1, 2, 4
CARD_KINDS = ("y", "y2", "r")


def cstr(ram, addr, cap):
    raw = ram[addr:addr + cap]
    end = raw.find(b"\0")
    raw = raw[:end if end >= 0 else cap]
    return raw.decode("latin-1") if raw and all(32 <= b < 127 for b in raw) else ""


def engine_of(ram, scratch):
    """(executável do motor, mapa) - pelo nome do executável ou, nos patches
    que o renomearam (World Cup 26: "WC26_5MX2"), por dois pedaços do código
    do motor."""
    serial = ram[SERIAL:SERIAL + 11].decode("latin-1", "replace")
    m = ENGINES.get(serial)
    if m is None:
        for name, eng in ENGINES.items():
            if all(ram[a:a + 16] == bytes.fromhex(h) for a, h in eng["sig"]):
                serial, m = name, eng
                break
    return serial, (None if m and m["scratch"] and not scratch else m)


def u16(mem, addr):
    return struct.unpack_from("<H", mem, addr)[0]


def b(ram, sc, addr):
    """Byte de um campo do mapa: da RAM ou, com SCR, do scratchpad (sc)."""
    if addr & SCR:
        return sc[addr & 0x3FF] if sc else 0
    return ram[addr]


def pointer(ram, addr):
    (ptr,) = struct.unpack_from("<I", ram, addr)
    return ptr & 0x1FFFFF if 0x80000000 <= ptr < 0x80000000 + len(ram) else None


def capture_names(ram, st, m):
    """Nomes e códigos de todos os times enquanto a tela de seleção está na
    RAM, ou None: só vale a tabela inteira, do time 0 até depois dos dois
    times escolhidos agora."""
    names, codes = [], []
    for team in range(TEAMS):
        name_at = pointer(ram, m["sel_names"] + 4 * team)
        code_at = pointer(ram, m["sel_codes"] + 4 * team) if m["code_ptrs"] else m["sel_codes"] - 4 * team
        if name_at is None or code_at is None or (not m["code_ptrs"] and ram[code_at + 3] != 0):
            break
        name, code = cstr(ram, name_at, 24)[:16], cstr(ram, code_at, 4)
        if not name or (not m["mixed_case"] and name.upper() != name) or len(code) != 3:
            break
        names.append(name)
        codes.append(code)
    need = max(st[m["team_id"]], st[m["team_id"] + 1])
    return (names, codes) if len(names) >= 8 and len(names) > need else None


def read_state(ram, st, m, teams=None):
    """Estado da partida na RAM (st = RAM ou scratchpad, onde ficam time,
    período, relógio e placar), ou None se não parece uma partida desse motor.
    teams: os times do pontapé inicial (end_resets)."""
    period, clock = st[m["period"]], u16(st, m["clock"])
    pads = list(ram[m["pads"]:m["pads"] + 4]) if m["pads"] else list(NO_PADS)
    if period > 4 or any(p not in (0, 1, 0xFF) for p in pads):
        return None
    out = []
    size, gs = m["name_len"], m["goal_size"]
    for side in (0, 1):
        team_id, goals = st[m["team_id"] + side], st[m["score"] + side]
        if team_id >= TEAMS or goals > 30:
            return None
        if teams:
            team_id = teams[side]
        roster = m["roster"][0] + m["roster_team"] * team_id if m["roster_team"] else m["roster"][side]
        scorers = []
        base = m["goals"][side]
        for n in range(32):
            scorer, assist, minute = ram[base + gs * n:base + gs * n + 3]
            if minute == 0xFF:  # fim da lista
                break
            if (scorer != 0xFF and scorer >= m["roster_len"]) or minute > 200:
                return None
            if scorer == 0xFF:  # gol contra: na lista do time que ganhou o gol, sem o jogador
                scorers.append(["Gol contra", minute, ""])
                continue
            scorers.append([cstr(ram, roster + size * scorer, size), minute,
                            cstr(ram, roster + size * assist, size) if assist < m["roster_len"] else ""])
        out.append({"id": team_id, "name": "", "code": "", "goals": goals, "scorers": scorers})
    return {"period": period, "clock": clock, "pads": pads, "teams": out,
            "in_play": ram[m["in_play"]] if m["in_play"] else 1,
            "extra": u16(st, m["extra"]) if m["extra"] else None,
            "phase": b(ram, st, m["phase"]) if m["phase"] else None,
            "pens": [b(ram, st, m["pens"]), b(ram, st, m["pens"] + 1)] if m["pens"] else None,
            "kicker": b(ram, st, m["kicker"]) if m["kicker"] else None,
            "state": b(ram, st, m["state"]) if m["state"] else None,
            "in_match": b(ram, st, m["in_match"]) if m["in_match"] else 1,
            "settings": ram[m["settings"]] if m["settings"] else None}


class MatchWatch:
    """Pontapé inicial, gols, troca de tempo e fim - o que o fork faz por frame."""

    def __init__(self, content):
        self.content = content
        self.active = False
        self.kickoff = 0
        self.half = 18000
        self.armed = 0          # kick_on_run: relógio armado para o pontapé
        self.teams = None       # end_resets: os times do pontapé inicial
        self.over_since = 0     # extra: frame em que o tempo do 2º tempo acabou
        self.extra = False      # na prorrogação (o WE2002 a joga como períodos 0 e 1 de novo)
        self.last = None        # último estado lido
        self.last_msg = None    # end_resets: o estado da amostra anterior
        self.sent_key = None    # o que mudou desde o último envio
        self.sent_frame = 0
        self.names = None       # (nomes, códigos) da última tela de seleção
        # goal_watch(), a cada frame: autor do gol contra e gol de pênalti.
        self.watching = False
        self.last_touch = 0
        self.pen_frame = 0
        self.goal_count = [0, 0]
        self.goal_og = [[0] * 32, [0] * 32]   # gol contra: índice do autor + 1 (0 = não se sabe)
        self.goal_pen = [[False] * 32, [False] * 32]
        self.so_state, self.so_since = None, 0   # disputa de pênaltis: placar + estado, e desde quando
        self.et_goals, self.gg_clock = 0, 0        # prorrogação: gols vistos e o relógio + 1 do último (gol de ouro)
        self.decided = False                       # a amostra anterior: disputa decidida ou gol de ouro
        self.prev_phase, self.prev_score = 0, [0, 0]
        self.over_frame = 0                        # estado 10 com empate no 2º tempo / prorrogação

    def goal_watch(self, frame, ram, scratch=b""):
        """A cada frame (como o fork): quem tocou por último e se há um
        pênalti sendo cobrado, para o gol que acabou de aparecer."""
        serial, m = engine_of(ram, scratch) if self.active else (None, None)
        if not m or not m["touch"]:
            return
        touch = b(ram, scratch, m["touch"])
        if 1 <= touch <= 22:
            self.last_touch = touch
        if m["setpiece"] and b(ram, scratch, m["setpiece"]) == m["pen_value"] and (
                m["pen_state"] is None or b(ram, scratch, m["state"]) == m["pen_state"]):
            self.pen_frame = frame
        gs = m["goal_size"]
        for side in (0, 1):
            base = m["goals"][side]
            n = 0
            while n < 32 and ram[base + gs * n + 2] != 0xFF:
                n += 1
            for i in range(self.goal_count[side] if self.watching else n, n):
                own = ram[base + gs * i] == 0xFF
                self.goal_og[side][i] = (self.last_touch - 1) % 11 + 1 if own and self.last_touch else 0
                self.goal_pen[side][i] = bool(not own and self.pen_frame and frame - self.pen_frame <= PEN_FRAMES)
            for i in range(n, min(self.goal_count[side], 32)):
                self.goal_og[side][i], self.goal_pen[side][i] = 0, False
            self.goal_count[side] = n
        self.watching = True

    def decorate(self, st, ram, m):
        """Gol contra com o autor (do elenco do outro lado, no lugar da
        assistência) e gol de pênalti: o 4º item ("og"/"pen") do estado v2."""
        if not st or not m["touch"]:
            return st
        teams = [dict(t, scorers=[list(g) for g in t["scorers"]]) for t in st["teams"]]
        for side, team in enumerate(teams):
            for i, g in enumerate(team["scorers"]):
                if g[0] == "Gol contra" and self.goal_og[side][i]:
                    # o WE3 tem um elenco por time, não por lado
                    other = st["teams"][1 - side]["id"]
                    roster = m["roster"][0] + m["roster_team"] * other if m["roster_team"] else m["roster"][1 - side]
                    team["scorers"][i] = ["Gol contra", g[1],
                                          cstr(ram, roster + m["name_len"] * (self.goal_og[side][i] - 1), m["name_len"]), "og"]
                elif g[0] != "Gol contra" and self.goal_pen[side][i]:
                    team["scorers"][i] = g[:3] + ["pen"]
        return dict(st, teams=teams)

    def _start(self, mem, m, kickoff, half):
        # Só partida montada pelos jogadores: um controle em cada lado (o DEMO, CPU x CPU, não tem).
        if m["pads"]:
            pads = self._ram[m["pads"]:m["pads"] + 4]
            if 0 not in pads or 1 not in pads:
                self.active, self.armed = False, 0
                return
        # O WE3 não tem a tabela, mas marca o DEMO.
        if m["demo"] and self._ram[m["demo"]] == m["demo_value"]:
            self.active, self.armed = False, 0
            return
        self.so_seen = False  # a partida chegou aos pênaltis (o placar deles vai daí em diante)
        self.run_phase = 0    # a fase + 1 em que o relógio foi visto andando por último
        self.drop_frame = 0   # WE4: a bola parou sem o apito do juiz, desde quando
        self.active, self.kickoff, self.half, self.armed, self.over_since = True, kickoff, half, 0, 0
        self.whistle = 0  # o apito inicial (estado 1: bola rolando)
        self.extra = False
        self.teams = [mem[m["team_id"]], mem[m["team_id"] + 1]]
        self.last_msg = None
        # o placar desta partida a partir daqui (pontapé novo não é placar diminuindo)
        self.prev_score = [mem[m["score"]], mem[m["score"] + 1]]
        self.et_goals, self.gg_clock, self.decided, self.so_since = sum(self.prev_score), 0, False, 0
        self.card_bits = [[0] * 32, [0] * 32]  # por índice do elenco: os cartões já listados
        self.cards, self.injuries = [[], []], [[], []]
        self.injury_seen = 0
        # Pontapé inicial: os cartões e contusões que o jogo ainda tem (partida reiniciada)
        # são de antes dele - só a partida em que se entrou no meio os lista.
        if kickoff and (m["cards"] or m["injuries"]):
            self.events(self._ram, m, None, quiet=True)

    def minute(self, phase, clock):
        """O minuto como o jogo grava nos gols: 0-44 o 1º tempo, 45-89 o 2º, 90-104 e
        105-119 a prorrogação (os acréscimos ficam no último minuto do tempo). Os tempos
        da prorrogação são um terço dos da partida (self.half depois de ver o pontapé dela)."""
        half = self.half or 18000
        if phase >= 2 and not self.extra:
            half = half // 3 or 1
        span = 15 if phase >= 2 else 45
        if phase >= 4:
            return 120
        base = 90 + 15 * (phase - 2) if phase >= 2 else 45 * phase
        run = half - clock if clock < half else 0
        return base + ((span * run - 1) // half if run else 0)

    def events(self, ram, m, st, quiet=False):
        """A cada amostra da partida: os cartões e contusões que apareceram desde a
        anterior, neste minuto - a RAM só tem o cartão atual de cada jogador.
        quiet: só os dá como vistos."""
        minute = 0 if quiet else self.minute(st["phase"] or 0, st["clock"])
        for side in (0, 1) if m["cards"] else ():
            roster = m["roster"][side]
            for i in range(min(m["roster_len"], 32)):
                v = ram[m["cards"][side] + i]
                want = (CARD_YELLOW if v & 3 else 0) | (v & CARD_SECOND) | (v & CARD_RED)
                add = want & ~self.card_bits[side][i]
                if not add or v > 7:  # outros bits: não são os cartões (fora de uma partida)
                    continue
                self.card_bits[side][i] |= add
                for k in range(3) if not quiet else ():
                    if add & (1 << k) and len(self.cards[side]) < EVENTS:
                        self.cards[side].append([cstr(ram, roster + m["name_len"] * i, m["name_len"]), minute,
                                                 CARD_KINDS[k]])
        self.injury_events(ram, m, st, minute, quiet)

    def injury_events(self, ram, m, st, minute, quiet=False):
        """Os contundidos: a lista só cresce na partida (cada registro fica até o fim; o
        jogo a limpa ao carregar a partida e depois do fim). O registro é {índice no
        elenco, time, 0, 0} na cruz amarela - a vermelha (o jogador sai de campo) ainda
        não foi vista: todas vão como cruz amarela por ora."""
        if not m["injuries"]:
            return
        teams = [t["id"] for t in st["teams"]] if st else []
        i = self.injury_seen
        while i < INJURY_RECORDS:
            rec = ram[m["injuries"] + 4 * i:m["injuries"] + 4 + 4 * i]
            if rec[0] == 0xFF:
                break  # o fim da lista
            if not quiet and rec[0] < m["roster_len"] and rec[1] in teams:
                side = 0 if rec[1] == teams[0] else 1  # o mesmo time dos dois lados: LOCAL
                if len(self.injuries[side]) < EVENTS:
                    self.injuries[side].append([cstr(ram, m["roster"][side] + m["name_len"] * rec[0], m["name_len"]),
                                                minute, "y"])
            i += 1
        self.injury_seen = i

    def _message(self, serial, frame, st, final):
        shootout = st["phase"] == 4
        over = st.get("state")
        st = {k: v for k, v in st.items() if k not in ("in_play", "extra", "phase", "kicker", "pens", "state")}
        if over is not None:
            st["over"] = int(over == STATE_OVER)  # o juiz encerrou o tempo: "Intervalo" só daí
        pens = self.last["pens"] if self.last else None
        if pens and (shootout or getattr(self, "so_seen", False)):
            st["pens"] = list(pens)
        st["teams"] = [dict(t) for t in st["teams"]]
        m = ENGINES[serial]
        for side, team in enumerate(st["teams"]):
            # WE2002: os números da tela RESULT (o WE Camp mostra no fim), cartões e contusões.
            if m["stats"]:
                s = [self._ram[m["stats"] + side + 2 * i] for i in range(6)]
                team["stats"] = {"shots": s[0], "offsides": s[1], "fouls": s[2], "yellow": s[4], "red": s[3],
                                 "corners": s[5]}
            if m["cards"]:
                team["cards"] = [list(e) for e in self.cards[side]]
            if m["injuries"]:
                team["injuries"] = [list(e) for e in self.injuries[side]]
        if self.names:
            for team in st["teams"]:
                if team["id"] < len(self.names[0]):
                    team["name"], team["code"] = self.names[0][team["id"]], self.names[1][team["id"]]
        if final:
            st["period"] = 4 if shootout else st["period"]
        elif self.last and self.last["phase"] is not None:
            st["period"] = self.last["phase"]  # WE2002: a própria fase
        elif self.extra:
            st["period"] += 2  # prorrogação
        return {"v": 2, "engine": serial, "content": self.content, "kickoff": self.kickoff, "frame": frame,
                "half": self.half, "final": int(final), **st}

    def sample(self, frame, ram, scratch=b""):
        """Chamado a cada SAMPLE_FRAMES; devolve o estado a mandar, ou None."""
        serial, m = engine_of(ram, scratch)
        self._ram = ram
        mem = (scratch if m["scratch"] else ram) if m else None
        if m and not self.active:
            self.names = capture_names(ram, mem, m) or self.names
        st = self.decorate(read_state(ram, mem, m), ram, m) if m else None
        prev, self.last = self.last, st
        if st is None:
            self.active = False
            return None
        final = final_before = False
        if prev is not None:
            # Pontapé inicial do 1º tempo: o relógio salta para a duração do tempo
            # (de 0, ou - WE4, depois de sair pelo menu de pausa - de onde ele seguia
            # descendo). O WE3 arma o relógio na tela de formação: o pontapé é
            # quando ele começa a correr.
            if st["period"] == 0 and st["clock"] >= MIN_KICKOFF_CLOCK and st["clock"] >= prev["clock"] + MIN_KICKOFF_CLOCK:
                # Prorrogação: logo depois do fim do 2º tempo (período 1, relógio 0) da
                # partida em andamento, o WE2002 começa outro "1º tempo" (período 0,
                # relógio menor) com o placar mantido - a mesma partida, períodos 2 e 3.
                # Na fase 2 (já marcada) a amostra pode cair no período 0 com o relógio ainda em 0.
                if self.active and ((prev["period"] == 1 and prev["clock"] == 0) or st["phase"] in (2, 3)):
                    self.extra, self.half = True, st["clock"]
                elif m["kick_on_run"]:
                    self.active, self.armed = False, st["clock"]
                else:
                    self._start(mem, m, frame, st["clock"])
            # WE3: a formação de uma partida (in_match vai a 1) arma o relógio - que pode não
            # saltar muito (depois de sair pela pausa ele seguia descendo nos menus).
            elif (m["kick_on_run"] and m["in_match"] and st["in_match"] and not prev["in_match"]
                  and st["period"] == 0 and st["clock"] >= MIN_KICKOFF_CLOCK):
                self.active, self.armed = False, st["clock"]
            elif self.armed and st["period"] == 0 and st["clock"] < prev["clock"]:
                self._start(mem, m, frame, self.armed)
            # Entrou no meio (retry-connect): o relógio já está correndo. Não com o
            # estado da partida (WE2002): a tela de seleção de times também tem um
            # contador descendo (partida fantasma) - o fork só deixa logo depois do go-live.
            elif not self.active and not m["state"] and 0 < st["clock"] < prev["clock"] and st["in_play"]:
                self._start(mem, m, 0, 18000)
            elif self.active and m["in_play"] and m["state"]:
                # WE4: a bola para no intervalo e no fim (o estado do juiz, abaixo); sem ele,
                # saíram pela pausa, em qualquer tempo. O apito pode vir logo depois da bola parar.
                if prev["in_play"] and not st["in_play"] and st["state"] != STATE_OVER:
                    self.drop_frame = frame
            elif self.active and m["in_play"]:
                # Sem o estado: a bola para depois do 2º tempo com o relógio em 0 = fim; com o relógio correndo = saiu.
                if prev["in_play"] and not st["in_play"] and st["period"] == 1:
                    if st["clock"] == 0:
                        final = True
                    else:
                        self.active = False
            # WE2002, WE3: depois do 2º tempo, o período volta a 0 com o relógio zerado
            # (o WE3 zera o placar junto: o fim é a amostra anterior).
            # WE2002: o período também volta a 0 quando o empate vai para a prorrogação
            # (fase 2, já marcada) ou para os pênaltis (fase 4).
            # (O "continuar" do WE3: o período pode vir da prorrogação ou dos pênaltis.)
            elif (self.active and st["period"] == 0 and st["clock"] == 0 and not m["end_resets"]
                  and prev["period"] == 1 and st["phase"] in (2, 4)):
                pass
            elif self.active and st["period"] == 0 and st["clock"] == 0 and (
                    prev["period"] == 1 or (m["end_resets"] and prev["period"] >= 1)):
                if not m["end_resets"]:
                    final = True
                elif prev["clock"] == 0 and self.last_msg:
                    final_before = True
                else:
                    self.active = False
            if getattr(self, "drop_frame", 0) and (st["in_play"] or st["state"] == STATE_OVER):
                self.drop_frame = 0
            elif getattr(self, "drop_frame", 0) and frame - self.drop_frame >= LEFT_FRAMES:
                self.drop_frame, self.active = 0, False  # saíram da partida
            # WE3, prorrogação: períodos 2 e 3, um terço do tempo - o "half" do card acompanha.
            if self.active and st["period"] in (2, 3) and st["clock"] >= prev["clock"] + MIN_KICKOFF_CLOCK:
                self.half = st["clock"]
        score = [mem[m["score"]], mem[m["score"] + 1]]
        # WE2002: o juiz encerrou o tempo. 2º tempo / prorrogação com vencedor, gol de
        # ouro (relógio da prorrogação andando) ou disputa decidida = fim; empate: a
        # prorrogação ou os pênaltis vêm a seguir, ou em DRAW_FRAMES terminou empatado.
        # Todos os motores também mostram o estado entre os tempos com o próximo período já
        # marcado (relógio em 0, ou já armado por alguns frames - no WE2002 por 1-2 frames: uma
        # amostra ali era um falso fim no intervalo): fim de tempo tem o relógio andado até 0
        # nesse período; gol de ouro, o relógio andado (abaixo do tempo), com a opção ligada.
        ph = st["phase"]
        if self.active and st["clock"] > 0:
            self.run_phase = (ph or 0) + 1
        sets = st["settings"] or 0
        time_up = ph in (1, 3) and st["clock"] == 0 and getattr(self, "run_phase", 0) == ph + 1
        # Gol de ouro no WE2002 (sem as opções na RAM): em qualquer tempo da prorrogação.
        golden = ph in (2, 3) and 0 < st["clock"] < self.half and (not m["settings"] or bool(sets & SET_GOLDEN))
        if self.active and m["state"] and not final and not final_before and st["state"] == STATE_OVER:
            if ph == 4:
                final = final or st["pens"][0] != st["pens"][1]
            else:
                # Empate: o WE3 sabe pelas opções o que vem.
                if time_up or golden:
                    if score[0] != score[1]:
                        final = True
                    elif m["settings"]:
                        if not ((ph == 1 and sets & SET_EXTRA) or sets & SET_PENS):
                            final = True  # empate: nem prorrogação nem pênaltis
                    elif not self.over_frame:
                        self.over_frame = frame
                    elif frame - self.over_frame >= DRAW_FRAMES:
                        final = True
        else:
            self.over_frame = 0
        # WE2002, prorrogação: gol depois do qual o relógio não anda mais = gol de ouro.
        if self.active and st["phase"] in (2, 3):
            if sum(score) > self.et_goals:
                self.gg_clock = st["clock"] + 1
            elif self.gg_clock and st["clock"] + 1 != self.gg_clock:
                self.gg_clock = 0  # a partida seguiu
            self.et_goals = sum(score)
        elif self.active:
            self.et_goals, self.gg_clock = sum(score), 0
        if self.active and st["phase"] == 4:
            self.so_seen = True
        # O jogo saiu da partida - menus (outros times), "reiniciar" (a fase volta
        # a 0 com os mesmos times, depois o placar) ou placar diminuindo: disputa
        # decidida ou gol de ouro acabam assim (o fim é a amostra anterior); senão
        # a partida foi abandonada, e o placar zerado nunca vai.
        # O WE3 diz quando os jogadores saíram da partida (in_match).
        if self.active and not final and not final_before and prev is not None and (
                (not m["end_resets"] and ([mem[m["team_id"]], mem[m["team_id"] + 1]] != self.teams
                                          or score[0] < self.prev_score[0] or score[1] < self.prev_score[1]
                                          or (self.decided and st["phase"] is not None
                                              and st["phase"] < self.prev_phase)))
                or (m["in_match"] and prev["in_match"] and not st["in_match"])):
            if self.decided and self.last_msg:
                final_before = True
            else:
                self.active = False
        self.decided = st["phase"] is not None and (
            (st["phase"] == 4 and st["pens"][0] != st["pens"][1])
            or (st["phase"] in (2, 3) and self.gg_clock > 1 and score[0] != score[1])
            or (st["state"] == STATE_OVER and (st["phase"] == 4 or time_up or golden)))
        self.prev_phase, self.prev_score = st["phase"] or 0, score
        # WE2002: a disputa acaba quando o estado dela (9 nas cobranças, 11/13 por
        # ~6 s depois de um erro) vira FF ou, decidida, fica no valor de um erro.
        if self.active and not final_before and st["phase"] == 4 and st["kicker"] is not None:
            so = (st["pens"][0], st["pens"][1], st["kicker"])
            if so != self.so_state or not self.so_since:
                self.so_state, self.so_since = so, frame
            if st["pens"][0] != st["pens"][1] and (st["kicker"] == 0xFF or (
                    st["kicker"] not in (0, 9) and frame - self.so_since >= SHOOTOUT_IDLE)):
                final = True
        # WE3 sem o estado: relógio e acréscimo do 2º tempo em 0 por 10 s = fim.
        if self.active and m["extra"] and not m["state"] and not final and not final_before:
            if st["period"] == 1 and st["clock"] == 0 and st["extra"] == 0:
                if not self.over_since:
                    self.over_since = frame
                elif frame - self.over_since >= OVER_FRAMES:
                    final = True
            else:
                self.over_since = 0
        if not self.active:
            return None
        if m["cards"] or m["injuries"]:
            self.events(ram, m, st)
        # Os primeiros segundos depois do apito inicial: nada vai (podem sair e recomeçar).
        # WE2002: do apito (estado 1); os outros: do relógio armado.
        if m["state"] and not getattr(self, "whistle", 0) and st["state"] == 1:
            self.whistle = frame
        if not final and not final_before and (
                (not getattr(self, "whistle", 0) or frame - self.whistle < GRACE_FRAMES) if m["state"]
                else (self.kickoff and frame - self.kickoff < GRACE_FRAMES)):
            return None
        if final_before:
            self.active = False
            return dict(self.last_msg, final=1)
        if m["end_resets"]:
            st = self.decorate(read_state(ram, mem, m, self.teams), ram, m)  # com os times do pontapé inicial
            self.last_msg = self._message(serial, frame, st, False)
        elif st["phase"] is not None:  # a amostra antes do fim (saiu da partida decidida)
            self.last_msg = self._message(serial, frame, st, False)
        # + o relógio do tempo começando a correr (o card sai do "Intervalo").
        # + um cartão / uma contusão novos (as estatísticas só vão junto).
        key = json.dumps([st["period"], st["pads"], st["teams"], st["phase"], st["pens"], st["clock"] >= self.half,
                          st["state"] == STATE_OVER, [len(x) for x in getattr(self, "cards", [[], []])],
                          [len(x) for x in getattr(self, "injuries", [[], []])]])
        if not final and key == self.sent_key and frame - self.sent_frame < HEARTBEAT_FRAMES:
            return None
        self.sent_key, self.sent_frame = key, frame
        if final:
            self.active = False
            st = dict(st, period=1, clock=0)  # o placar fica; no WE2002 o período já voltou a 0
        return self._message(serial, frame, st, final)


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
        if watch.active:
            watch.goal_watch(f, core.ram(), core.scratch())
        if f % SAMPLE_FRAMES:
            continue
        st = watch.sample(f, core.ram(), core.scratch())
        if watch.active and paced_from is None:
            paced_from = (f, time.time())  # do pontapé inicial em diante, no ritmo pedido
        if paced_from and a.speed > 0:
            ahead = (f - paced_from[0]) / 60.0 / a.speed - (time.time() - paced_from[1])
            if ahead > 0:
                time.sleep(ahead)
        if st is None:
            continue
        t1, t2 = st["teams"]
        line = "%6d %s per %d relogio %5d  %s %d x %d %s%s  %s" % (
            f, SP.KH.fmt_time(f), st["period"], st["clock"], t1["name"] or t1["id"], t1["goals"], t2["goals"],
            t2["name"] or t2["id"], "  FIM" if st["final"] else "",
            [[g for g in t["scorers"] if len(g) > 3] for t in st["teams"]])
        if a.dry:
            print(line, flush=True)
            continue
        replies = [post(a.url, a.key, st, players, r, room) for r in (players if a.both else players[:1])]
        print(line, " -> ", " | ".join(replies), flush=True)
    print("fim em %.0f s" % (time.time() - t0))


if __name__ == "__main__":
    main()
