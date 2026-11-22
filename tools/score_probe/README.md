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
`SLPM_861.62` = Winning Eleven 3 Final Version, `SLPM_868.35` = J.League
Jikkyou Winning Eleven 2001 e `SLPM_866.00` = Winning Eleven 2000 U-23 (os
dois últimos com o mapa no fim deste arquivo). O formato do arquivo (.bin,
.chd) não muda nada. Patch que renomeou o executável (World Cup 26 PepeWE:
`WC26_5MX2`) é reconhecido por dois pedaços de 16 bytes do código do motor
(WE 2002, J.League 2001 e WE2000 U-23: 0x8001701C e 0x8001D038; WE4: 0x80017034 e 0x8001D03C; WE3:
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
| Opções da partida: bit 0 prorrogação, bit 1 gol de ouro, bit 2 pênaltis (copiadas da tela de ajustes quando a partida carrega; ficam numa revanche) | 0x80072B30 (na tela de ajustes: 0x8010D460 / 64 / 68, palavras) | 0x80072FD2 | 0x8010A307 |
| Dentro de uma partida (da formação até sair dela; 0 = saíram) | — | — | 0x1F800198 |
| Menu de pausa: aberto (1; o Start que pula um replay e o intervalo não o abrem) / o controle que o abriu, 0 = controle 1 ("PAUSE JOYPAD 1"; só ele mexe no menu) / o item do cursor, 0 = o 1º - o EXIT MATCH é o 6. O menu que fecha com o cursor no EXIT MATCH é o "Sim" dele: o Start e o X o fecham com o cursor já de volta no 1º item (2 quadros antes; no WE3 o cursor fica onde estava, e vale a saída da partida logo depois) | 0x80061CD0 / 0x80075501 / 0x80075500 | 0x8005291C / 0x80075099 / 0x80075098 | 0x800E6B78 / 0x800FF6E0 / 0x800E7D8A (na RAM) |
| DEMO da tela de título (CPU x CPU) | controles FF | controles FF | 0x8010A2C2 = 1 |
| Números da tela RESULT, pares LOCAL / VISITA: chutes, impedimentos, faltas, vermelhos (o 2º amarelo não conta), amarelos, escanteios, faltas cobradas, pênaltis | a partir de 0x800756F2 (até 0x80075701; o placar vem logo depois) | a partir de 0x80075283 (até 0x80075292; o placar vem logo depois) | a partir de 0x801085B0 (na RAM; o placar fica no scratchpad) |
| Cartão de cada jogador, um byte por índice do elenco: 1 amarelo, 2 o 2º amarelo (expulso), 4 vermelho direto; zera depois do fim da partida | LOCAL 0x80075568 / VISITA 0x8007557F (+ índice) | LOCAL 0x800750FF / VISITA 0x80075115 (+ índice), os mesmos valores | LOCAL 0x80107F8D / VISITA 0x80107FA3 (+ índice no elenco do time), os mesmos valores |
| Chutes de cada jogador (somam os chutes da tela RESULT) | LOCAL 0x80075596 / VISITA 0x800755AD (+ índice) | LOCAL 0x8007512B / VISITA 0x80075141 (+ índice) | LOCAL 0x80107FB9 / VISITA 0x80107FCF (+ índice) |
| Contundidos: registros de 4 bytes `ID do jogador (u16, veja a linha de baixo), 0, 0` (cruz amarela), FF = o fim da lista; só cresce na partida, o jogo limpa 10 registros ao carregar a partida e depois do fim (0x80073966 foi a 1 no PW26 e ficou em 0 no Euro: não é a quantidade) | 0x80073968 | não procurado | não procurado |
| ID de cada jogador do elenco, u16 `índice nos dados do jogo, time dos dados`: 22 do LOCAL, os 22 da VISITA logo depois. Numa seleção é `índice no elenco, ID do time`; um time do Master League pega jogadores dos dados de outros times (Master Liga 12: WARLEY, time 94, tem `0..21, 57` - o Koller contundido é `8, 57`, e o time 57 é o "82 FRANCA") | 0x800649F8 (VISITA 0x80064A24) | não procurado | não procurado |

Pontapé inicial: com o período em 0, o relógio salta para a duração do tempo
(de 0 ou, no WE4 depois de sair pelo menu de pausa, de onde ele seguia
descendo nos menus). No WE3 o salto é na tela de formação (e, depois de uma
partida, antes da seleção dos times da próxima): o pontapé é quando o
relógio começa a descer.

Fim (desde 2026-11-08, nos três motores): o estado vai a 10 com a fase 1 ou 3
e o relógio andado até 0 naquele tempo (vencedor = fim; empate = prorrogação
ou pênaltis se as opções mandarem, senão fim empatado já no apito; até
2026-11-17 o WE 2002, sem as opções mapeadas, esperava 25 s - quem fechava o
jogo antes deixava o empate "Interrompido"), com a fase 2/3 e o relógio parado no meio (gol
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

Opções da partida nos 5 motores (2026-11-17), na tela de ajustes de um
amistoso (1P x CPU), jogada no krec_harness com o controle "na mão" (liga a
ISO, aperta os botões, tira screenshots, salva o state): as combinações 0, 1,
2, 3, 4, 5 e 7 ligadas no menu, depois a partida carregada, deram o byte de
bits igual em todos - WE 2002 (o "WE2002.bin" Clássicos Mundiais) 0x80072B30,
WE4 Traducida 0x80072FD2, WE3 Final Ver. (Eng) 0x8010A307, J.League 2001
0x8006F288 e WE2000 U-23 0x80072A90. Ligar só o gol de ouro liga a
prorrogação junto (dá 3); no WE 2002, desligar a prorrogação apaga o gol de
ouro. No WE 2002 os IDs dos times 6 bytes depois (0x80072B36/37) vão a FF no
fim da partida, mas as opções ficam: numa partida até o fim seguida de
revanche (sem passar pela tela de ajustes) o byte seguiu em 7. No replay da
Copa 9152-1794954103 (ProfOneTwo x UelberLWS, ISO oficial da Copa, 379
janelas iguais às dos jogadores) eles desligaram as três no menu (0x8010D460..68
de 1 para 0) e o byte ficou em 0 nos dois jogos - o 2º pela revanche.

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

## J.League Jikkyou Winning Eleven 2001 (`SLPM_868.35`)

O motor do WE 2002 um ano antes: as mesmas estruturas e os mesmos valores,
em outros endereços, com **26 jogadores por time** (o WE 2002 tem 23). Achado
em 2026-11-15 no replay 14340-1794770038 (TIERES_ART x TIERES, sem M. Card,
"J.League Jikkyou Winning Eleven 2001 (Japan-Eng).chd", conteúdo
40353FBD:CF0CE77): Kashima Antlers 2 x 4 Yokohama FC, partida de 5 minutos
(9000 frames por tempo), com dois gols de pênalti, um gol contra, 7 cartões
(dois 2º amarelos e três vermelhos diretos) e duas contusões - tudo
conferido na tela e na tela RESULT.

| O quê | Endereço |
|---|---|
| ID do time LOCAL / VISITA | 0x80070F9C / 9D |
| Período | 0x80070F9E |
| Relógio (u16, como o do WE 2002) | 0x800713A4 |
| Gols LOCAL / VISITA | 0x8007114C / 4D (gols de cada tempo nos 4 bytes seguintes de cada lado: 0x7114E LOCAL, 0x71152 VISITA) |
| Gols: 32 registros de 4 bytes, como no WE 2002 | 0x80071008 / 0x80071088 |
| Elencos: 26 x 16 bytes (nome até 11 letras + NUL; depois do NUL sobra lixo) | 0x80060D40 / 0x80060EE0 |
| Controles 1-4 (0 LOCAL, 1 VISITA, FF sem time; cópias em +4 e +8) | 0x80071325 |
| Nome curto de cada time em inglês ("ANTLERS", "YOKOHAM"; sem código de 3 letras): ponteiros, time 0 primeiro - só se abrirem a tela de formação, e dali até o 1º tempo (sem ela, o fork usa os 41 nomes do jogo, os mesmos) | 0x80159F50 |
| Opções da partida, bits como no WE3/WE4 (bit 0 prorrogação, 1 gol de ouro, 2 pênaltis), gravadas ao carregar a partida; os IDs dos times vêm 4 bytes depois (cópia em 0x80077511) - o mesmo bloco do WE2000 | 0x8006F288 |
| Estado da partida (os valores do WE 2002; 10 no intervalo e no fim) | 0x800711CE |
| Fase (0/1 tempos, 2/3 prorrogação, 4 pênaltis) / placar dos pênaltis, como no WE 2002 | 0x80071158 / 0x80071156 |
| Último toque (posição 1-22, como no WE 2002) | 0x800772C9 |
| Pênalti sendo cobrado (2 até o gol; também estado 2 com 0x800711CF = 3) | 0x80076F21 |
| Números da tela RESULT (a mesma ordem do WE 2002; o placar vem logo depois) | 0x8007113C |
| Cartão de cada jogador (1, 2, 4) | LOCAL 0x80070FA0 / VISITA 0x80070FBA |
| Chutes de cada jogador | LOCAL 0x80070FD4 / VISITA 0x80070FEE |
| Contundidos: registros `índice, ID do time, ?, 0`, `FF FF` = fim; 10 registros | 0x800701E6 |
| Menu de pausa: aberto / o controle que o abriu / o item do cursor (EXIT MATCH = 6), como no WE 2002. Depois do EXIT MATCH nada da partida muda (o relógio até continua andando): o "Sim" é o único sinal da saída | 0x8005E451 / 0x80070F39 / 0x80070F38 |

No 3º byte de um contundido, o primeiro (Nakata K.) passou a 1 quando o
segundo se machucou; não se sabe o que é (o fork o ignora, como no WE 2002).
Prorrogação e pênaltis conferidos no replay 25008-1794776316 (TIERES_ART x
TIERES, mesma ISO, prorrogação e pênaltis ligados, gol de ouro desligado):
S-Pulse 0 x 0 Sagan Tosu, pênaltis 4 x 5. A prorrogação é como a do WE 2002
(fase 2/3, tempos de um terço do normal: 3000 frames); a disputa, fase 4
com o placar dos pênaltis, e o estado vai a 10 no fim dela. As opções foram
achadas nessa tela de ajustes, que eles ligaram e desligaram várias vezes:
no menu ficam em 0x800EDF70/74/78 (palavras), apagadas quando a partida
carrega; na partida, o byte de bits 0x8006F288 (0, 5 e 7 nas três sessões;
trocar as do menu no harness no frame 7500 deu 0 a 7, como no WE2000). Os 3
bytes antes dos IDs dos times (0x70F99..9B), tomados antes pelas opções, não
são elas: deram 0 0 1 com tudo ligado no replay 25008-1794784942. Nessa partida
ninguém abriu a tela de formação: a tabela de nomes nunca esteve na RAM (por
isso o fork tem os nomes do jogo).
Replay 25008-1794784942 (tudo ligado, 5 minutos): o DEMO da tela de título
(controles em FF), duas partidas contra a CPU seguidas - a segunda pelo
"reiniciar", com prorrogação e pênaltis - (só o controle 1 no LOCAL) e
Cerezo 1 x 0 Montedio entre os dois, decidida pelo gol de ouro do
Morishima aos 119' - nos acréscimos do 2º tempo da prorrogação (a tela
mostra LOSS TIME; o relógio chegou a 0 em 124200, o gol veio em 125180):
estado 7, 11 e 10 no apito (126240). Com o relógio em 0, o fork fecha pela
regra do tempo esgotado com vencedor, no mesmo apito em que fecharia pela do
gol de ouro (que exige o relógio correndo - vista no WE2000). O fork não
manda nada no DEMO nem contra a CPU; a partida dos dois sai inteira, com a
tela RESULT conferida (números, cartões, a contusão do Tasaka).
Ainda não vistos: um gol de ouro com o relógio da prorrogação ainda correndo
(a mesma regra do WE2000) e o byte do estado da disputa (o 0x7571C do WE 2002 - não faz falta: o
estado do juiz a encerra).

O shim C do fork deu, no replay 14340-1794770038: pontapé no frame 6720, os
6 gols com os dois pênaltis (Goto 29', Bismark 44') e o gol contra do
Takakuwa (88'), os cartões e as contusões, e o fim no frame 38100 (o apito em
38080); no 25008-1794776316: os dois tempos, a prorrogação, cada cobrança da
disputa e o fim no frame 47550 com os pênaltis 4 x 5, os cartões (Tominaga
amarelo e 2º amarelo, Koishi vermelho) e os números da tela RESULT. O
live_sim.py manda o mesmo nos três (46, 59 e 56 estados iguais).

## Winning Eleven 2000 U-23 (`SLPM_866.00`)

O motor do WE 2002 dois anos antes: as mesmas estruturas e os mesmos valores,
em outros endereços, com **22 jogadores por time**. Achado em 2026-11-15 em
dois replays do Walter (TIERES_ART x TIERES, sem M. Card, "WE2000 - U-23
(Japan-Ptbr).chd", patch "Desafio U-23" em português, conteúdo
3016A56C:11149CA5), conferidos na tela FINAL:
- 25008-1794777311: Paraguai 4 x 4 EUA, prorrogação e pênaltis 6 x 5, um
  gol contra (Caniza, que o jogo mostra depois do gol), um gol de pênalti
  (McBride 36'), 6 amarelos e 1 vermelho para cada lado;
- 25008-1794778410: Japão 0 x 0 Brasil, pênaltis 0 x 3 sem prorrogação,
  expulsões e a contusão do Zé Roberto (falta do Nanami, 2º amarelo).

| O quê | Endereço |
|---|---|
| ID do time LOCAL / VISITA | 0x800750AC / AD |
| Período | 0x800750AE |
| Relógio (u16; 9000 por tempo numa partida de 5 minutos, 3000 na prorrogação) | 0x80075460 |
| Gols LOCAL / VISITA | 0x80075244 / 45 (gols de cada tempo nos 4 bytes seguintes de cada lado) |
| Gols: 32 registros de 4 bytes, como no WE 2002 | 0x80075108 / 0x80075188 |
| Elencos: 22 x 16 bytes | 0x80064518 / 0x80064678 |
| Controles 1-4 (0 LOCAL, 1 VISITA, FF sem time; cópias em +4 e +8) | 0x8007541D |
| Só na tela de seleção: ponteiro para o nome de cada time (em português, "ESTADOS UNIDOS") | 0x800BD590 |
| Só na tela de seleção: código de 3 letras, 4 bytes cada, time 0 aqui, descendo | 0x8007FAA0 |
| Estado da partida (os valores do WE 2002) | 0x800752C6 |
| Fase / placar dos pênaltis | 0x80075250 / 0x8007524E |
| Último toque (posição 1-22, como no WE 2002) | 0x8007B385 |
| Pênalti sendo cobrado (2 até o gol) | 0x8007AFDD |
| Opções da partida, bits como no WE3/WE4 (bit 0 prorrogação, 1 gol de ouro, 2 pênaltis), gravadas ao carregar a partida; os IDs dos times vêm 4 bytes depois (cópia em 0x8007B5C1) | 0x80072A90 |
| Números da tela FINAL (a mesma ordem do WE 2002; o placar vem logo depois) | 0x80075234 |
| Cartão de cada jogador (1, 2, 4) | LOCAL 0x800750B0 / VISITA 0x800750C6 |
| Chutes de cada jogador | LOCAL 0x800750DC / VISITA 0x800750F2 |
| Contundidos: registros `índice, ID do time, 0, 0`, `FF FF` = fim; 10 registros (o WE 2002 menos 0x102) | 0x80073866 |
| Menu de pausa: aberto / o controle que o abriu / o item do cursor (SAIR = 6), como no WE 2002 | 0x80061BB0 / 0x80075049 / 0x80075048 |

As opções da tela de ajustes ficam em 0x80112D88 / 8C / 90 (palavras, até a
partida carregar). Trocá-las no harness logo antes da carga (frame 4250 do
primeiro replay) deu 1, 2, 4, 3, 5, 0 e 7 em 0x72A90 - assim os bits foram
conferidos. No J.League a mesma troca (0x800EDF70/74/78, frame 7500 do
replay 25008-1794776316) deu 0 a 7 em 0x6F288: os mesmos bits.

O shim C do fork deu, nos dois replays, os placares, os 8 gols da primeira
partida com a assistência, o gol contra e o pênalti, a prorrogação, cada
cobrança das disputas e o fim no apito (60600 e 42300), os cartões, a
contusão e os números da tela FINAL; o live_sim.py manda o mesmo (83 e 50
estados iguais).

Gol de ouro no replay 25008-1794779900 (opções 7): Espanha 4 x 3 Inglaterra,
Raul 115' no 2º tempo da prorrogação (fase 3, relógio em 845). O estado vai
a 7 no gol, 11 na comemoração e 10 no apito (~430 frames depois), com o
relógio parado: o fork manda o fim ali, pela regra do gol de ouro com a
opção ligada. Tela FINAL conferida (gols, minutos e números).

DEMO e partida contra a CPU no replay 25008-1794784085: o DEMO da tela de
título (Japão x Brasil) tem os 4 controles em FF; a partida contra a CPU
(Nigéria x República Tcheca), só o controle 1 no LOCAL (`00 FF FF FF`). Nos
dois o relógio salta para 9000 e o fork vê o pontapé, mas não abre a partida
(ksync_live_start exige um controle em cada lado): 0 estados no replay
inteiro, também nos menus e na seleção de times.
