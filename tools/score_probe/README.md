# score_probe

Procura variáveis do jogo (placar, estatísticas) na RAM do PS1 reproduzindo
replays `.krec` com o `krec_harness` (mesmo core, mesmas opções do fork).
Precisa de numpy e pillow (um venv à parte serve).

- `score_probe.py scan`   – conta mudanças por byte e guarda os candidatos a contador (`cand.npy`).
- `score_probe.py record` – série dos candidatos a cada `--every` frames (`record.npz`).
- `score_probe.py snap`   – RAM inteira nos frames de `--at` (`ram_<frame>.bin`).
- `score_probe.py series` – valores de `--addr` ao longo da partida + conferência dos digests `[SYNC]`.
- `score_probe.py shots`  – PNGs da tela nos frames de `--at` (`a:b:passo` vale).
- `crops.py` / `montage.py` – recortes do placar / grade de capturas, com o frame em cada uma.
- `match_pattern.py` – filtra `record.npz` pelo placar visto na tela.

Partidas com M. Card Online: o cartão de cada slot (hash no marcador
`[M. Card online] <slot1> <slot2>`) fica no servidor em
`/var/www/arena17/memcards/<sha256>.mcd`. Copie para
`<--out>/harness_saves/pcsx-card1.mcd` / `pcsx-card2.mcd` e passe
`--opt pcsx_rearmed_memcard1=shared` (slot vazio = `none`, o padrão). O jogo
grava no cartão durante a partida: restaure o original antes de cada execução.

- `card.py` – monta o "card ao vivo" (times, controles, placar, tempo, autores dos gols) de um `ram_<frame>.bin`.
- `--swap` troca os inputs dos jogadores 1 e 2 (desde o boot o jogo muda: alguns menus só aceitam o controle 1).

## Mapa da RAM (motor do WE 2002)

Conferido em 2026-11-07 com replays de Copa Africana CAN 2025 (9704, 11980) e
Euro vs Copa America 2004 (12492, 3076, 9828), sempre contra a tela.
Lado 0 = LOCAL do jogo, lado 1 = VISITA. O local do jogo não é
necessariamente o mandante do campeonato.

| Endereço | O quê |
|---|---|
| 0x80075564 / 65 | ID do time LOCAL / VISITA (índice da tabela de nomes) |
| 0x80075566 | período: 0 = 1º tempo, 1 = 2º; volta a 0 depois do apito final (tela RESULT) |
| 0x80075702 / 03 | gols LOCAL / VISITA (zeram no início da partida seguinte) |
| 0x800755C4 / 0x80075644 | gols LOCAL / VISITA: 32 registros de 4 bytes `autor, assistência (FF = sem), minuto, 0`; FF = fim |
| 0x800756F2.. | estatísticas em pares (F2/F3 parecem finalizações) |
| 0x8007591C (u16) | quadros que faltam no tempo; começa em duração/2 × 3600 (18000 numa partida de 10 min); minuto = 45 × (1 − restante/inicial) + 45 × período; 0 = acréscimos |
| 0x80064570 / 0x800646E0 | elencos LOCAL / VISITA: 23 nomes de 16 bytes (índice = autor/assistência) |
| 0x800758D9..DC | controles 1-4: 0 = LOCAL, 1 = VISITA, FF = sem time (cópias em 0x758DD e 0x758E1) |
| 0x8015DBC4 | ponteiros para os nomes dos times – só está na RAM em alguns menus, e patches como o CAN não renomeiam (slot 16 "FINLAND" = Camarões) |

Fim de partida: o período volta de 1 para 0 com o relógio em 0 e o placar
mantido; a partida seguinte começa com o relógio em 18000 e o placar 0 x 0.
Ainda não visto: prorrogação, pênaltis (placar e período), partida contra a CPU.
