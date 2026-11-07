"""Filtra os candidatos de record.npz por um padrão de placar visto na tela.

match_pattern.py <record.npz> <inicio> <fim> "<frame:valor>,..."  [outro padrão ...]
O padrão diz o valor esperado a partir de cada frame (o gol aconteceu entre a
última captura com o valor antigo e a primeira com o novo, então os frames
de cada mudança vêm em pares: 33601:0,34800:1 = o byte vira 1 em algum ponto
entre 33601 e 34800). Fora dos intervalos de transição, o valor tem que bater.
"""
import sys

import numpy as np

path, start, end = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
d = np.load(path)
cand, frames, values = d["cand"], d["frames"], d["values"]
sel = (frames >= start) & (frames <= end)
fr, vals = frames[sel], values[sel]

for pattern in sys.argv[4:]:
    points = sorted((int(a), int(b)) for a, b in (p.split(":") for p in pattern.split(",")))
    expected = np.full(len(fr), -1)
    for i, f in enumerate(fr):
        # Valor do último ponto <= f, desde que o próximo ponto (se houver) tenha o mesmo valor
        # ou f esteja antes da janela de transição.
        prev = [v for pf, v in points if pf <= f]
        nxt = [v for pf, v in points if pf > f]
        if prev and (not nxt or nxt[0] == prev[-1]):
            expected[i] = prev[-1]
    mask = expected >= 0
    ok = np.all(vals[mask] == expected[mask][:, None], axis=0)
    hits = cand[ok]
    print("padrão %s: %d endereço(s)" % (pattern, len(hits)))
    for a in hits[:40]:
        col = values[:, np.nonzero(cand == a)[0][0]]
        changes = [(int(frames[i]), int(col[i])) for i in range(len(col)) if i == 0 or col[i] != col[i - 1]]
        print("  0x%08X  mudanças: %s" % (0x80000000 + int(a), changes[:12]))
