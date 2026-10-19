# wecamp_test

Testa o `common/n02_wecamp.cpp` (cliente da conta WE Camp / Memory Card online
do DLL) contra um wg-camp de verdade rodando nesta máquina, sem precisar
publicar nada.

- `local_wgcamp.py` sobe o wg-camp (`C:\TIERES\wg-camp`) com um banco
  temporário, duas contas já confirmadas e a mesma chave de espectador do DLL.
- `wecamp_client.cpp` faz o caminho de uma partida: senha errada, login dos
  dois jogadores, checkout (slot 1 = 1P, slot 2 = 2P), download do cartão pelo
  token e pela chave do DLL, recusa de quem não está na lista ou não tem conta,
  os dois envios de fim de partida (pendente → gravado), conflito com base
  antiga, e logout.
- `n02.ini` (criado à mão, ao lado do .exe) aponta o cliente para o servidor
  local - o DLL usa as mesmas chaves para testes:

```
[SC]
WECAMP_HOST=127.0.0.1
WECAMP_PORT=5055
WECAMP_HTTP=1
```

## Como rodar

```
build.cmd
C:\TIERES\wg-camp\.venv\Scripts\python.exe local_wgcamp.py C:\TIERES\wg-camp --port 5055 --fake-discord
wecamp_client.exe Pele Mtgamess segredo123
```

O servidor fica em primeiro plano - rode o cliente em outro terminal.

## mc_session_test

Roda o código de verdade do M. Card online do DLL (a `McSession` do
`kailleraclient.cpp`, copiada pelo `extract_mc_region.py` no `build.cmd`) com
uma sala simulada, contra o mesmo wg-camp local: o jogo gravando o cartão
setor por setor, o envio do save durante a partida (só depois que o cartão
fica 3 s parado), a linha "Memory Card Online Slot X foi salvo." no chat
(saindo pela thread do jogo), o envio do fim da partida, a partida sem save
(nada enviado) e o convidado, que só anota os saves e envia 15 s depois do
fim (o host caiu).

```
mc_session_test.exe
```

Ele usa o mesmo `n02.ini` e se recusa a rodar se o `WECAMP_HOST` não for
`127.0.0.1` - sem o `n02.ini`, o DLL falaria com o WE Camp de produção.

Também testa os ingressos das salas "Só logados WE Camp": pedido pelo
jogador, conferido pelo host, recusado em outra sala, de uso único, e falso.

## Resultado (2026-10-08)

31 verificações, todas OK.

Canal de voz do Discord (`--fake-discord`, o Discord falso dos testes do
wg-camp): jogador já em voz é movido, jogador fora de voz recebe o link do
mesmo canal, sala sem o jogador e cliente desconectado são recusados.

## Resultado (2026-10-09)

53 verificações, todas OK (31 + 7 da opção "Participar da chamada de voz" + o canal de voz: o primeiro pedido espera, o segundo cria o canal).

## Resultado (2026-10-14)

66 verificações, todas OK. As novas são do "M. Card da ISO" (um cartão só do
jogo, igual para todos):
- o host logado e um convidado sem login (com a chave do DLL) recebem o mesmo
  cartão, num slot só;
- sem login, o M. Card Online continua recusado;
- o host salva no fim da partida, e o envio igual do convidado não muda nada;
- uma outra partida ao mesmo tempo vira conflito, e quem não está na partida
  é recusado;
- a próxima partida começa da versão salva.

Depois, mais 9 do M. Card Online com uma conta só: o host sem login confere o
ingresso do convidado com a chave do DLL; o host sem login e o convidado
logado recebem os mesmos slots (slot 1 vazio, slot 2 = cartão do convidado);
sala sem nenhuma conta é recusada. Total: 75 verificações, todas OK.

## Resultado (2026-10-15)

83 verificações, todas OK. O "M. Card da ISO" passou a ter um cartão para cada
slot: o checkout do host e o do convidado recebem os dois slots (o DLL pede
`iso_slots=2`); salvar o slot 1 não mexe no slot 2; o slot 2 é salvo com
`slot=2`, o envio igual do convidado não muda nada, um slot que não existe é
recusado, e a próxima partida começa das versões salvas dos dois slots.

Neste PC (Python 3.14 do E:\Projetos), o `local_wgcamp.py` precisou de dois
ajustes: ler o `n02_stream.h` como Latin-1 e não perguntar o nome da máquina
(`socket.getfqdn`). Para compilar aqui, o `vcvars64.bat` é o do VS 2019
BuildTools (`C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools`).

Depois, os saves durante a partida: o checkout traz `multi_base=1` e o
SHA-256 que o DLL calcula é o mesmo do WE Camp - 85 verificações, todas OK.
O `mc_session_test`: 20 verificações, todas OK.

## Resultado (2026-10-19)

102 verificações, todas OK. As novas são do token que o site não conhece
mais ("Desconectar" em outro RetroArch que tinha o mesmo `n02.ini`): o pedido
de ingresso é negado com a mensagem "sua conexao ... caiu" e o login é
esquecido (memória e `n02.ini`); o checkout, o download e o envio do
"M. Card da ISO" e a conferência de ingresso do host vão de novo com a chave
do DLL; e, se outra janela da mesma pasta já logou de novo, só o token morto
é esquecido - o login novo fica no `n02.ini`. O `wecamp_client` agora também
se recusa a rodar se o `n02.ini` não apontar para `127.0.0.1`.
O `mc_session_test`: 18 verificações, todas OK.
