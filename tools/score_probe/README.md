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

## Mapas da RAM

O jogo é reconhecido pelo nome do executável que o kernel do PS1 guarda em
**0x80009E19** (o mesmo endereço em todos os jogos; os patches mantêm o do
motor): `SLPM_870.56` = Winning Eleven 2002, `SLPM_862.91` = Winning Eleven 4.
O formato do arquivo (.bin, .chd) não muda nada. Lado 0 = LOCAL do jogo (não
necessariamente o mandante do campeonato).

| O quê | WE 2002 (`SLPM_870.56`) | WE4 (`SLPM_862.91`) |
|---|---|---|
| ID do time LOCAL / VISITA | 0x80075564 / 65 | 0x800750FB / FC |
| Período (0 = 1º tempo, 1 = 2º) | 0x80075566 | 0x800750FD |
| Relógio: u16, quadros que faltam no tempo (no pontapé inicial = duração do tempo) | 0x8007591C | 0x800754A8 |
| Bola rolando (1) / parada no intervalo e no fim (0) | — | 0x800754B9 |
| Gols LOCAL / VISITA | 0x80075702 / 03 | 0x80075293 / 94 |
| Gols: 32 registros de 4 bytes `autor, assistência (FF = sem), minuto, 0`, FF = fim | 0x800755C4 / 0x80075644 | 0x80075157 / 0x800751D7 |
| Elencos LOCAL / VISITA (nome em N bytes, índice = autor/assistência) | 0x80064570 / 0x800646E0, 23 x 16 | 0x8007490C / 0x800749E8, 22 x 10 |
| Controles 1-4: 0 = LOCAL, 1 = VISITA, FF = sem time (cópias em +4 e +8) | 0x800758D9 | 0x80075469 |
| Só na tela de seleção: ponteiro para o nome de cada time | 0x800BF0E8 | 0x800A67C0 (nomes em inglês; a tela mostra em japonês) |
| Só na tela de seleção: código de 3 letras | os próprios códigos, 4 bytes cada, time 0 em 0x800800DC, descendo | ponteiros em 0x800A6AC0 |

Pontapé inicial (os dois): com o período em 0, o relógio salta para a duração
do tempo (de 0 ou, no WE4 depois de sair pelo menu de pausa, de onde ele
seguia descendo nos menus).

Fim:
- WE 2002: depois da tela RESULT o período volta de 1 para 0, relógio em 0.
- WE4: o período fica em 1; a bola para (0x800754B9 de 1 para 0) com o
  relógio em 0. Parar com o relógio correndo = saíram pelo menu de pausa
  (partida abandonada).
- Relógio em 0 com a bola rolando = acréscimos (dá gol aos 90+).

Conferido em 2026-11-07, sempre contra a tela, com replays reproduzidos sem
divergir dos digests dos jogadores:
- WE 2002: Euro vs Copa America 2004 (12492, 3076, 9828), Copa Africana CAN
  2025 (9704, 11980) e BR Turbo 2025 em .chd (15056).
- WE4: Winning Eleven 4 Traducida .chd (12560: Irlanda 7 x 0 Portugal; 16036:
  Portugal x França abandonada pelo menu, depois França 2 x 1 Portugal).

O código C do fork também foi testado nesses replays inteiros, compilado numa
DLL que o harness chama a cada 30 frames, como o RetroArch faz.

Ainda não visto: prorrogação, pênaltis (placar e período), partida contra a
CPU e Winning Eleven 3 Final Version (sem replay com essa ISO).
