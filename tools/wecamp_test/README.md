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
C:\TIERES\wg-camp\.venv\Scripts\python.exe local_wgcamp.py C:\TIERES\wg-camp --port 5055
wecamp_client.exe Pele Mtgamess segredo123
```

O servidor fica em primeiro plano - rode o cliente em outro terminal.

Também testa os ingressos das salas "Só logados WE Camp": pedido pelo
jogador, conferido pelo host, recusado em outra sala, de uso único, e falso.

## Resultado (2026-10-08)

31 verificações, todas OK.
