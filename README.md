# Switch Latency Monitor (10GbE Hardware Timestamping)

Ferramenta em C++ para medicao de latencia de switches Ethernet utilizando **Hardware Timestamping (PHC)** e **Raw Sockets (`AF_PACKET`)**, sem necessidade de namespaces ou regras complexas de roteamento.

## Arquivos Inclusos
- `switch_latency_monitor.cpp`: Codigo-fonte principal em C++17.
- `Makefile`: Automacao de compilacao.
- `plot_latency.py`: Script Python para geracao dos graficos a partir do CSV exportado.

## Como Compilar

Em Ubuntu ou Debian, instale as ferramentas de compilação com `sudo apt install build-essential`. Para medir entre dois hosts, compile o projeto nos dois.

```bash
make
```

## Medição em um único host

### Preparar as interfaces

No modo local, as interfaces de medição dispensam configuração de IP.
Certifique-se de que as duas portas da placa 10GbE estejam fisicamente ativas:
```bash
sudo ip link set eth0 up
sudo ip link set eth1 up
```

### Sincronização dos relógios PTP (PHC)

O monitor calcula a latência subtraindo o timestamp de hardware de TX do timestamp de hardware de RX. Quando as portas usam relógios distintos, a diferença entre eles entra no valor medido.

Consulte as capacidades e o índice do relógio de cada porta. Se necessário, instale o `ethtool` com `sudo apt install ethtool` no Ubuntu ou Debian.

```bash
ethtool -T eth0
ethtool -T eth1
```

Substitua `eth0` e `eth1` pelos nomes das suas interfaces. O campo `PTP Hardware Clock` identifica o dispositivo. Por exemplo, o índice `2` corresponde a `/dev/ptp2`. Se as duas interfaces estiverem associadas ao mesmo PHC, elas já usam a mesma referência de tempo.

Se os índices forem diferentes, instale o pacote que fornece o `phc2sys`. No Ubuntu ou Debian, execute:

```bash
sudo apt install linuxptp
```

Por exemplo, com `enp134s0f0` (TX) em `/dev/ptp2` e `enp134s0f1` (RX) em `/dev/ptp3`, execute:

```bash
sudo phc2sys -s /dev/ptp2 -c /dev/ptp3 -O 0 -m
```

`-s` seleciona o relógio de referência, `-c` seleciona o relógio que será ajustado, `-O 0` define um desvio alvo de zero e `-m` mostra o estado da sincronização no terminal.

Acompanhe o valor `offset`, exibido em nanossegundos, e aguarde o estado `s2` com um desvio estável e compatível com a precisão desejada. Mantenha o `phc2sys` rodando em um terminal e execute o monitor em outro. [Manual do phc2sys](https://www.linuxptp.org/documentation/phc2sys/)

### Executar o modo local
```bash
sudo ./switch_latency_monitor eth0 eth1 latency_results.csv
```
Pressione `Ctrl+C` a qualquer momento para encerrar e salvar o relatorio final.

## Medição entre dois hosts

O Host A transmite os quadros e exibe o dashboard. O Host B registra os timestamps de hardware na recepção. Ao final de cada lote, A envia o intervalo de sequências pelo canal de controle TCP e B aguarda uma janela de recepção de 10 ms antes de devolver os registros. A atualiza as estatísticas e então começa o próximo lote.

### Sincronizar os hosts

Use interfaces de medição na mesma rede Ethernet, com suporte a timestamp por hardware em TX no Host A e ao filtro RX `all` no Host B. Confira com `ethtool -T` em cada host e instale `linuxptp` nos dois. Em uma rede de teste isolada, rode o PTP em camada 2 com o relógio de A como referência e B como cliente.

```bash
# Host A
sudo ip link set enp134s0f0 up
ethtool -T enp134s0f0

# Host B
sudo ip link set enp134s0f1 up
ethtool -T enp134s0f1
```

```bash
# Host A — interface de medição
sudo ptp4l -i enp134s0f0 -2 -H -m

# Host B — interface de medição
sudo ptp4l -i enp134s0f1 -2 -H -s -m
```

Aguarde o estado sincronizado e confira o offset no Host B. Mantenha o `ptp4l` rodando nos dois hosts e confira se o offset continua estável depois de iniciar o monitor, que configura o filtro de timestamp das interfaces. O tráfego PTP continua durante o teste. Uma assimetria entre ida e volta no caminho PTP afeta a precisão da medição. [Manual do ptp4l](https://www.linuxptp.org/documentation/ptp4l/)

### Executar a medição por lotes

Use o IPv4 de controle do Host B e libere a porta TCP **46000** de A para B. O canal de controle troca automaticamente os endereços MAC das interfaces de medição. Se houver uma rede de gerenciamento separada, use o IPv4 dessa rede para manter o controle fora do caminho medido.

No Host B, use `ip -4 addr show dev eno1` para conferir se a interface de controle tem um endereço. No Host A, `ip route get 192.168.50.2` mostra por qual interface o sistema envia o tráfego de controle. Substitua o nome da interface e o IP pelos do seu ambiente.

```bash
# Host B — inicie o receptor e informe a interface de controle
sudo ./switch_latency_monitor --rx enp134s0f1 eno1

# Host A — use o IPv4 que o receptor exibiu, por exemplo 192.168.50.2
sudo ./switch_latency_monitor --tx enp134s0f0 192.168.50.2 latency_hosts.csv 600
```

O receptor mostra o endereço de escuta e o comando para conectar o Host A. Você também pode passar o IPv4 local diretamente ao `--rx`, inclusive para escolher um endereço quando a interface tem mais de um IPv4.

Para usar a mesma interface no Host B para medição e controle, informe o nome dela nos dois parâmetros:

```bash
sudo ./switch_latency_monitor --rx enp134s0f1 enp134s0f1
```

A interface precisa ter um IPv4 acessível pelo Host A. Use o endereço que o receptor exibir no comando `--tx` do Host A. Nesse arranjo, os quadros de medição e o controle TCP passam pelo mesmo switch, e o receptor devolve os timestamps entre os lotes.

Substitua as interfaces e o endereço de exemplo pelos valores do seu ambiente. O tamanho do lote é opcional e aceita de 1 a 10.000 quadros, com padrão de 600. O CSV usa o mesmo formato do modo local.

Ao fechar um lote, os quadros enviados que B não registrou entram na contagem de perda. Quadros que chegaram sem timestamp válido contam como recebidos e ficam fora das estatísticas de latência. Se a resposta de controle falhar ou expirar, A encerra a sessão com o lote inconclusivo e preserva os lotes já gravados. Use `Ctrl+C` no Host A para encerrar. O CSV contém os lotes concluídos.

## Como gerar os gráficos

Instale `pandas` e `matplotlib`. No Ubuntu ou Debian, use `sudo apt install python3-pandas python3-matplotlib`. Execute o script no host onde o CSV foi gravado.

```bash
# Medição local
python3 plot_latency.py latency_results.csv grafico_latencia_switch.png

# Medição entre dois hosts — execute no Host A
python3 plot_latency.py latency_hosts.csv grafico_latencia_hosts.png
```
