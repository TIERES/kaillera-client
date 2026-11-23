# kmsg_game_test

Testa o que o `kcore/k_message.h` faz quando se perde uma rajada de pacotes do servidor no
meio de uma partida, contra um EmuLinker-K de verdade.

`kmsg_game_client.cpp` joga uma partida de 1 jogador. Para isso usa o `k_message.h` e o
`k_instruction` do próprio DLL, e o laço de jogo copia o `kaillera_modify_play_values()`:

- manda o input do frame;
- espera os dados do servidor e, enquanto espera, chama `resend_message(5)` a cada 10 ms;
- segue as mesmas regras de saída do jogo.

Cada input leva o número do frame. Numa partida de 1 jogador o servidor devolve exatamente o
nosso input, então os números recebidos têm de vir em sequência: 0, 1, 2... Um número pulado
é uma mensagem de jogo perdida (GAMEDATA/GAMCDATA). Numa partida de verdade, isso seria um
desync.

A perda é simulada no próprio socket: a partir de um frame, os próximos N datagramas do
servidor são descartados antes de chegar ao `k_message`.

## Como rodar

Primeiro suba um EmuLinker-K local. Pode ser o jar do fork
(`E:\Projetos\EmuLinker-K-RaM\emulinker\build\libs`) com `conf\` copiado de `release\` e
`controllers.connect.port` trocado. Precisa de Java 17.

Depois, no "x64 Native Tools Command Prompt", a partir desta pasta:

```
cl /nologo /EHsc kmsg_game_client.cpp ..\..\common\k_socket.cpp ws2_32.lib
kmsg_game_client 27950 hold 150 1800 300:6,500:8,700:10,900:12,1100:15,1300:25,1500:40
```

Argumentos: `<porta> <hold|skip> <atraso do pong em ms> <frames> <frame:pacotes,...>`.

- `hold`: o comportamento novo (`k_message::hold_holes` ligado durante o jogo).
- `skip`: o antigo, que pula o buraco depois de 250 ms.
- O atraso do pong aumenta o ping medido no login e, com ele, o frame delay. Com delay 1 a 4
  a redundância dos bundles (5 mensagens) já cobre a perda. O buraco só aparece com mais
  frames em trânsito: 150 ms dá delay 10.
- Os primeiros 2 s não são conferidos. O EmuLinker manda frames zerados no começo e descarta
  alguns dos primeiros inputs sozinho. Isso acontece sem perda nenhuma e com qualquer
  cliente: foi conferido também com um cliente Python independente.

Códigos de saída:

- 0 = todos os frames chegaram em ordem;
- 1 = input pulado ou errado;
- 2 = saiu da partida (perda que o servidor não reenviou);
- 3 = falha no login ou na sala.

## Resultado (2026-11-23, delay 10, rajadas de 6 a 40 pacotes)

| servidor                               | DLL            | resultado                                                             |
|----------------------------------------|----------------|-----------------------------------------------------------------------|
| fork ELK1.0.2.RA.0.3 (reenvio por serial) | `hold` (novo)  | 1790 inputs em ordem, 0 errados, 7 buracos recuperados                |
| EmuLinker sem o patch (RA.0.1)         | `hold` (novo)  | sai da partida na 1ª rajada, depois de 5 s                            |
| fork RA.0.3                            | `skip` (antigo) | 2 inputs errados (o desync silencioso de antes)                      |

Com delay 1 e 4, o modo novo também deu 0 errados, sem nenhum buraco.

- O servidor sem o patch registra um aviso "invalid message bundle" por pedido de reenvio:
  75 linhas em 5 s.
- O teste do login (`../kmsg_burst_test`) continua igual nos 4 cenários.
