# Switch Latency Monitor (10GbE Hardware Timestamping)

Ferramenta em C++ para medicao de latencia de switches Ethernet utilizando **Hardware Timestamping (PHC)** e **Raw Sockets (`AF_PACKET`)**, sem necessidade de namespaces ou regras complexas de roteamento.

## Arquivos Inclusos
- `switch_latency_monitor.cpp`: Codigo-fonte principal em C++17.
- `Makefile`: Automacao de compilacao.
- `plot_latency.py`: Script Python para geracao dos graficos a partir do CSV exportado.

## Como Compilar
```bash
make
```

## Preparacao das Interfaces (Sem precisar configurar IP)
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

## Como Executar
```bash
sudo ./switch_latency_monitor eth0 eth1 latency_results.csv
```
Pressione `Ctrl+C` a qualquer momento para encerrar e salvar o relatorio final.

## Como Gerar os Graficos
Instale as dependencias Python (`pandas` e `matplotlib`) e execute:
```bash
python3 plot_latency.py latency_results.csv grafico_latencia_switch.png
```
