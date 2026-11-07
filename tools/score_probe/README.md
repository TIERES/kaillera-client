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
motor): `SLPM_870.56` = Winning Eleven 2002, `SLPM_862.91` = Winning Eleven 4,
`SLPM_861.62` = Winning Eleven 3 Final Version. O formato do arquivo (.bin,
.chd) não muda nada. Patch que renomeou o executável (World Cup 26 PepeWE:
`WC26_5MX2`) é reconhecido por dois pedaços de 16 bytes do código do motor
(WE 2002: 0x8001701C e 0x8001D038; WE4: 0x80017034 e 0x8001D03C; WE3:
0x8001714C e 0x8001D19C - os bytes estão no fork e no live_sim.py), iguais
em todas as ISOs conferidas, nos menus e na partida. Lado 0 = LOCAL do jogo (não necessariamente o mandante
do campeonato).

O WE3 guarda a partida (times, período, relógio, placar) no **scratchpad**
do PS1 (1 KB em 0x1F800000), não na RAM principal: o fork o pega no mapa de
memória que o pcsx_rearmed publica, e o krec_harness também
(`Core.scratch()`; `score_probe.py snap` salva `scr_<frame>.bin`).

| O quê | WE 2002 (`SLPM_870.56`) | WE4 (`SLPM_862.91`) | WE3 FV (`SLPM_861.62`) |
|---|---|---|---|
| ID do time LOCAL / VISITA | 0x80075564 / 65 | 0x800750FB / FC | 0x1F8002DD / DE |
| Período (0 = 1º tempo, 1 = 2º) | 0x80075566 | 0x800750FD | 0x1F8002DF |
| Relógio: u16, quadros que faltam no tempo (no pontapé inicial = duração do tempo) | 0x8007591C | 0x800754A8 | 0x1F8002C4 |
| Acréscimos: u16, crescem no tempo e descem depois do relógio chegar a 0 | — | — | 0x1F8002C8 |
| Bola rolando (1) / parada no intervalo e no fim (0) | — | 0x800754B9 | — |
| Gols LOCAL / VISITA | 0x80075702 / 03 | 0x80075293 / 94 | 0x1F8002D1 / D2 |
| Gols: 32 registros `autor, assistência (FF = sem), minuto[, 0]`, FF = fim | 0x800755C4 / 0x80075644, 4 bytes | 0x80075157 / 0x800751D7, 4 bytes | 0x80107FE5 / 0x80108045, 3 bytes |
| Elencos (nome em N bytes, índice = autor/assistência) | LOCAL 0x80064570 / VISITA 0x800646E0, 23 x 16 | LOCAL 0x8007490C / VISITA 0x800749E8, 22 x 10 | por time: 0x800C8504 + time x 0xB0, 22 x 8 (japoneses em Shift-JIS: saem sem nome) |
| Controles 1-4: 0 = LOCAL, 1 = VISITA, FF = sem time (cópias em +4 e +8) | 0x800758D9 | 0x80075469 | não achado: 1P no LOCAL, 2P na VISITA (como a tela de seleção mostra) |
| Só na tela de seleção: ponteiro para o nome de cada time | 0x800BF0E8 | 0x800A67C0 (nomes em inglês; a tela mostra em japonês) | 0x801D1B48 (a tela mostra em japonês; o patch "Camera torre TV mode" tem os nomes em português) |
| Só na tela de seleção: código de 3 letras | os próprios códigos, 4 bytes cada, time 0 em 0x800800DC, descendo | ponteiros em 0x800A6AC0 | os próprios códigos, time 0 em 0x80189B58, descendo |

Pontapé inicial: com o período em 0, o relógio salta para a duração do tempo
(de 0 ou, no WE4 depois de sair pelo menu de pausa, de onde ele seguia
descendo nos menus). No WE3 o salto é na tela de formação (e, depois de uma
partida, antes da seleção dos times da próxima): o pontapé é quando o
relógio começa a descer.

Fim:
- WE 2002: depois da tela RESULT o período volta de 1 para 0, relógio em 0.
- WE4: o período fica em 1; a bola para (0x800754B9 de 1 para 0) com o
  relógio em 0. Parar com o relógio correndo = saíram pelo menu de pausa
  (partida abandonada).
- WE3: no 2º tempo, relógio e acréscimos em 0 por 10 s (o jogo às vezes dá
  mais 2 s depois do primeiro 0). O período só volta a 0 quando saem do menu
  depois do resultado, e o placar e os gols zeram junto - se isso vier antes,
  vale a amostra anterior. Nesse menu ("Trocar de time") os IDs dos times já
  mudam com o placar ainda lá: os times valem os do pontapé inicial.
- Relógio em 0 com a bola rolando = acréscimos (dá gol aos 90+).

Conferido em 2026-11-07, sempre contra a tela, com replays reproduzidos sem
divergir dos digests dos jogadores:
- WE 2002: Euro vs Copa America 2004 (12492, 3076, 9828), Copa Africana CAN
  2025 (9704, 11980) e BR Turbo 2025 em .chd (15056).
- WE4: Winning Eleven 4 Traducida .chd (12560: Irlanda 7 x 0 Portugal; 16036:
  Portugal x França abandonada pelo menu, depois França 2 x 1 Portugal).
- WE 2002 renomeado: WorldCup26PepeWEmxCamOriginal.chd (42668: Argentina x
  Japão, gol de pênalti do Messi - o registro do gol não marca pênalti, sai
  como gol sem assistência).
- WE3: WE3 Camera torre TV mode.bin, patch do Final Version (30928: Japão 5 x
  2 Nigéria e Brasil 1 x 3 Nigéria; 3512: Argentina 7 x 1 Holanda e 3 x 0).
  O Final Ver. (Eng) .chd tem o mesmo executável e os elencos nos mesmos
  endereços (nomes de time em inglês).

O código C do fork também foi testado nesses replays inteiros, compilado numa
DLL que o harness chama a cada 30 frames, como o RetroArch faz.

Ainda não visto: prorrogação, pênaltis (placar e período), partida contra a
CPU, gol contra (como o registro aparece) e, no WE3, sair pelo menu de pausa
e outro arranjo de controles.
