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
| Gols: 32 registros `autor, assistência (FF = sem), minuto[, 0]`, minuto FF = fim; autor FF = gol contra (na lista do time que ganhou o gol, sem o jogador) | 0x800755C4 / 0x80075644, 4 bytes | 0x80075157 / 0x800751D7, 4 bytes | 0x80107FE5 / 0x80108045, 3 bytes |
| Elencos (nome em N bytes, índice = autor/assistência) | LOCAL 0x80064570 / VISITA 0x800646E0, 23 x 16 | LOCAL 0x8007490C / VISITA 0x800749E8, 22 x 10 | por time: 0x800C8504 + time x 0xB0, 22 x 8 (japoneses em Shift-JIS: saem sem nome) |
| Controles 1-4: 0 = LOCAL, 1 = VISITA, FF = sem time (cópias em +4 e +8) | 0x800758D9 | 0x80075469 | não achado: 1P no LOCAL, 2P na VISITA (como a tela de seleção mostra) |
| Só na tela de seleção: ponteiro para o nome de cada time | 0x800BF0E8 | 0x800A67C0 (nomes em inglês; a tela mostra em japonês) | 0x801D1B48 (a tela mostra em japonês; o patch "Camera torre TV mode" tem os nomes em português) |
| Só na tela de seleção: código de 3 letras | os próprios códigos, 4 bytes cada, time 0 em 0x800800DC, descendo | ponteiros em 0x800A6AC0 | os próprios códigos, time 0 em 0x80189B58, descendo |
| Estado da partida: 0 saída de bola, 1 bola rolando, 2 falta/pênalti a cobrar, 7 gol, 8 disputa de pênaltis (WE3/WE4), 11 replay/comemoração, **10 o juiz encerrou o tempo** (intervalo, fim, gol de ouro, fim dos pênaltis) | 0x80075782 | 0x80075312 | 0x1F80029A (só vale com "dentro da partida" = 1) |
| Fase: 0/1 tempos, 2/3 prorrogação, 4 pênaltis | 0x8007570E | 0x8007529F | é o próprio período (0x1F8002DF) |
| Placar dos pênaltis LOCAL / VISITA | 0x8007570C / 0D | 0x8007529D / 9E | 0x1F8002DB / DC |
| Último toque na bola: posição 1-22 (1-11 LOCAL nas fases 0/2, VISITA nas 1/3); elenco = (posição - 1) % 11 - o autor do gol contra | 0x8007B841 | 0x8007B3DD | 0x1F8002AC |
| Pênalti sendo cobrado (até o chute; o gol vem ~25-30 frames depois) | 0x8007B499 = 2 | estado 2 e 0x80075313 = 3 | estado 2 e 0x1F80029B = 3 |
| Opções da partida: bit 0 prorrogação, bit 1 gol de ouro, bit 2 pênaltis | — | 0x80072FD2 | 0x8010A307 |
| Dentro de uma partida (da formação até sair dela; 0 = saíram) | — | — | 0x1F800198 |
| DEMO da tela de título (CPU x CPU) | controles FF | controles FF | 0x8010A2C2 = 1 |
| Números da tela RESULT, pares LOCAL / VISITA: chutes, impedimentos, faltas, vermelhos (o 2º amarelo não conta), amarelos, escanteios, faltas cobradas, pênaltis | a partir de 0x800756F2 (até 0x80075701; o placar vem logo depois) | a partir de 0x80075283 (até 0x80075292; o placar vem logo depois) | a partir de 0x801085B0 (na RAM; o placar fica no scratchpad) |
| Cartão de cada jogador, um byte por índice do elenco: 1 amarelo, 2 o 2º amarelo (expulso), 4 vermelho direto; zera depois do fim da partida | LOCAL 0x80075568 / VISITA 0x8007557F (+ índice) | LOCAL 0x800750FF / VISITA 0x80075115 (+ índice), os mesmos valores | LOCAL 0x80107F8D / VISITA 0x80107FA3 (+ índice no elenco do time), os mesmos valores |
| Chutes de cada jogador (somam os chutes da tela RESULT) | LOCAL 0x80075596 / VISITA 0x800755AD (+ índice) | LOCAL 0x8007512B / VISITA 0x80075141 (+ índice) | LOCAL 0x80107FB9 / VISITA 0x80107FCF (+ índice) |
| Contundidos: registros de 4 bytes `índice no elenco, ID do time, 0, 0` (cruz amarela), FF = o fim da lista; só cresce na partida, o jogo limpa 10 registros ao carregar a partida e depois do fim (0x80073966 foi a 1 no PW26 e ficou em 0 no Euro: não é a quantidade) | 0x80073968 | não procurado | não procurado |

Pontapé inicial: com o período em 0, o relógio salta para a duração do tempo
(de 0 ou, no WE4 depois de sair pelo menu de pausa, de onde ele seguia
descendo nos menus). No WE3 o salto é na tela de formação (e, depois de uma
partida, antes da seleção dos times da próxima): o pontapé é quando o
relógio começa a descer.

Fim (desde 2026-11-08, nos três motores): o estado vai a 10 com a fase 1 ou 3
e o relógio andado até 0 naquele tempo (vencedor = fim; empate = prorrogação
ou pênaltis se as opções mandarem, senão fim empatado - o WE 2002, sem as
opções na RAM, espera 25 s), com a fase 2/3 e o relógio parado no meio (gol
de ouro, com a opção ligada) ou com a fase 4 e os pênaltis diferentes. No WE3
e no WE4 o estado 10 aparece também entre os tempos com o próximo período já
marcado (relógio em 0 ou recém-armado) - por isso o "relógio andado".
Sair da partida: WE3 = "dentro da partida" vai a 0; WE4 = a bola para
(0x800754B9) sem o estado 10 por 1 s (pela pausa o estado fica em 1).

Antes (ainda vale sem o estado):
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

Conferido em 2026-11-08 com partidas jogadas por um robô (`webot.py`,
nesta pasta: liga a ISO no core, aperta os controles, salva/carrega estados e
grava .krec) - vitória, gols contra, gols de pênalti, prorrogação, gol de ouro,
disputa de pênaltis, empate, saída pela pausa e DEMO - no WE3 (.bin e Final
Ver. Eng .chd) e no WE4 Traducida.

Ainda não visto: partida contra a CPU, gol de ouro com a prorrogação
desligada, morte súbita longa nos pênaltis e, no WE3, a tabela de controles.

Estatísticas, cartões e contusões do WE 2002 (2026-11-11): a tela RESULT do
replay de teste no PW26 (232-1794366234, Brasil 0 x 5 Argentina, com 2
amarelos no Casemiro e 2 vermelhos diretos) e a do Euro 2004 em espanhol
bateram com a RAM; cartões em 12 replays de produção (só 1, 1→2 e 0→4; fora
de partida os bytes podem ter lixo, por isso só valem 0-7); contusões de cruz
amarela no PW26 (Enzo Fdez.) e no Euro (7160, uma em cada partida). O minuto
de um cartão/contusão é o da amostra, calculado como o jogo grava nos gols:
`base + (span x tempo andado - 1) / duração do tempo`, com base 0/45 e span 45
nos tempos normais, base 90/105 e span 15 na prorrogação (cujos tempos são
um terço dos normais); nos acréscimos fica no último minuto (44, 89).
A cruz vermelha (o jogador sai e não volta) ainda não foi vista: o fork manda
toda contusão como cruz amarela.

WE4 (2026-11-11): o mesmo desenho do WE 2002, achado no replay de teste do
Walter (26432-1794408657, Portugal 0 x 0 França no Traducida .chd, com 13
cartões forçados): Zidane amarelo e 2º amarelo, Rui Costa vermelho direto,
conferidos na tela; amarelos 5 x 7 e vermelhos 1 x 0 na RAM batem com os
cartões de cada jogador. Contusão não saiu (lista não achada).

WE3 Final Version (2026-11-11): o mesmo desenho, na RAM logo antes dos
registros de gols (0x80107FE5), achado no replay de teste do Walter
(26432-1794413909, Japão 0 x 0 Brasil no Final Ver. Eng .chd): Sampaio
amarelo, Leonardo amarelo e 2º amarelo, Cafu vermelho direto, conferidos na
tela; amarelos 6 x 4 e vermelhos 0 x 2 batem. O elenco do Japão está em
Shift-JIS (a tela mostra os nomes em inglês): os cartões dele saem sem nome,
como os gols. Contusão não saiu (lista não achada).
