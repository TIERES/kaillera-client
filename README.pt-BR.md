# N02 — Cliente Kaillera

[![Build](https://github.com/TIERES/kaillera-client/actions/workflows/build.yml/badge.svg)](https://github.com/TIERES/kaillera-client/actions/workflows/build.yml)

🇺🇸 [Read in English](README.md)

N02 é uma implementação de `kailleraclient.dll` que adiciona netplay a emuladores de N64 (feita originalmente para o emulador **RMG**, mas compatível com qualquer frontend que carregue um client Kaillera padrão). Suporta três modos de netplay:

- **P2P** — conexão direta ponto a ponto, sem necessidade de servidor.
- **Server (Kaillera)** — netplay tradicional via servidor Kaillera, compatível com servidores no estilo `kaillera.com` e a lista pública de servidores.
- **Playback** — reproduz partidas gravadas em `.krec`, e agora também permite **assistir partidas ao vivo** (veja abaixo).

## ✨ Novidade: Stream ao vivo!

Qualquer pessoa numa sala do Kaillera agora pode assistir a uma partida **enquanto ela ainda está rolando**, sem precisar entrar no jogo e sem precisar rodar o mesmo emulador que o host.

### Para o host: ative o "Stream ao vivo!"

Enquanto estiver hospedando uma sala, marque a opção **"Stream ao vivo!"**, ao lado de "Record game". Isso envia uma cópia ao vivo da partida (os mesmos dados de frame de input já gravados no seu `.krec` local) para um servidor comunitário de spectate, em segundo plano.

![Host ativando o "Stream ao vivo!" na sala de jogo](img/StreamAoVivo.png)

Todo mundo na sala também vê essa opção — desabilitada, já que só o host pode alterá-la — e recebe um aviso no chat sempre que o host liga ou desliga a stream, então ninguém precisa adivinhar se tem transmissão ao vivo disponível no momento. Quem entra na sala depois que a stream já estava ligada também é avisado automaticamente.

### Para quem quer assistir: "Acompanhar ao vivo!"

No lobby do servidor, qualquer sala com status **Playing** ganha uma opção extra no menu de clique direito: **"Acompanhar ao vivo!"**. Ela só aparece se a sala estiver realmente em partida — não precisa ficar checando a lista de jogos.

![Opção "Acompanhar ao vivo!" numa sala que está em partida](img/AcompanharAoVivo.png)

Ao clicar, o client procura a sessão ao vivo daquela sala e troca automaticamente para o modo Playback pra reproduzir a partida conforme ela acontece, com poucos segundos de atraso. Você não precisa ser jogador da partida, nem ter o mesmo build de emulador do host — você só está assistindo o replay do stream de input em tempo real.

Se o host ainda não tiver ativado o streaming (ou a stream ainda não começou a enviar dados), você recebe uma mensagem curta avisando disso, em vez da partida começar.

### Replays Online

Na tela de Playback, marque a caixa **"Replays Online"** pra trocar a lista de gravações locais pelas últimas partidas gravadas por qualquer jogador com pelo menos 5 minutos de duração, direto do servidor comunitário. Dando play numa delas, o client baixa o `.krec` pra sua pasta `records\` e já inicia a reprodução; o botão "Delete" vira "Download" nesse modo, pra baixar sem reproduzir. Desmarcando a caixa, volta pra lista local de sempre.

## Outros modos

- **Modo P2P** conecta dois jogadores diretamente via UDP — veja `core/p2p_core.cpp`.
- **Modo Server** conversa com um servidor no protocolo Kaillera pra gerenciar lobby/salas — veja `kcore/kaillera_core.cpp`.
- **Modo Playback** reproduz gravações `.krec` locais, ou uma stream ao vivo como descrito acima — veja `player.cpp`.

As gravações usam o formato `.krec`: um cabeçalho `KRC1` (nome do app, nome do jogo, timestamp, número do jogador, quantidade de jogadores e o nome de cada jogador) seguido por um fluxo de registros tipados (frames de input, chat, saída de jogador).

## Baixando o DLL

Pegue um `kailleraclient.dll` já compilado na página de [Releases](https://github.com/TIERES/kaillera-client/releases) — escolha **x64** ou **x86** de acordo com a arquitetura do seu emulador (a maioria dos emuladores modernos, incluindo o RMG, é x64).

Pra usar, copie o `kailleraclient.dll` baixado pra pasta do seu emulador, substituindo o que já existir lá.

## Compilando a partir do código-fonte

Requisitos: Windows, Visual Studio 2019+ com o workload de C++ (Windows SDK 10.0).

```bat
:: 64-bit (maioria dos emuladores)
build.bat

:: 32-bit
build-x86.bat
```

Os scripts detectam automaticamente o MSBuild instalado (VS2019/2022/2026) e geram o `kailleraclient.dll` em `x64\Release\` (64-bit) ou `Release\` (32-bit). O CI compila e publica automaticamente as duas arquiteturas a cada tag `v*`.

## Créditos

- Protocolo e API Kaillera: (c) 2001-2002 Christophe Thibault.
- N02 / N02.P2P: (c) Open Kaillera.
