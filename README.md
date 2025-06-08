# Projeto de Módulos do Kernel Linux

Bem-vindos ao projeto de implementação de módulos do kernel! Este repositório contém dois módulos do kernel Linux desenvolvidos para estender a funcionalidade do sistema operacional em tempo de execução, sem a necessidade de recompilar o kernel completo.

---

## Visão Geral do Projeto

Este projeto visa aprofundar o conhecimento em programação de nível de sistema, especificamente na criação de módulos de kernel para ambientes Ubuntu/Linux. Os módulos foram projetados para demonstrar a capacidade de estender o kernel e interagir com o espaço do usuário de forma controlada e eficiente.

---

### Pré-requisitos
- Kernel Linux com headers de desenvolvimento (ex: `linux-headers-$(uname -r)`)
- GCC

---

## Módulos Implementados

### 1. Módulo 1: `kfetch_mod` (Informações do Sistema)

Um driver de dispositivo de caractere que cria o dispositivo `/dev/kfetch`. Este módulo permite que programas do espaço do usuário recuperem informações detalhadas do sistema ao ler deste dispositivo.

#### **Funcionalidades:**

* **Informações do Sistema:** Retorna dados como:
    * **Kernel:** Versão do kernel.
    * **CPU:** Nome do modelo da CPU.
    * **CPUs:** Número de núcleos da CPU (online/total).
    * **Mem:** Informações de memória (livre/total em MB).
    * **Proc:** Número de processos em execução.
    * **Uptime:** Tempo de atividade do sistema em minutos.
* **Máscara de Informação:** Suporta uma **máscara de bits** (`Kfetch Information Mask`) para controlar quais informações são exibidas.
    ```c
    #define KFETCH_NUM_INFO 6
    #define KFETCH_RELEASE   (1 << 0) // Versão do kernel
    #define KFETCH_NUM_CPUS  (1 << 1) // Número de CPUs
    #define KFETCH_CPU_MODEL (1 << 2) // Modelo da CPU
    #define KFETCH_MEM       (1 << 3) // Informações de memória
    #define KFETCH_UPTIME    (1 << 4) // Tempo de atividade
    #define KFETCH_NUM_PROCS (1 << 5) // Número de processos
    #define KFETCH_FULL_INFO ((1 << KFETCH_NUM_INFO) - 1);
    ```
    Por exemplo, para exibir o nome do modelo da CPU e as informações de memória, a máscara seria `mask = KFETCH_CPU_MODEL | KFETCH_MEM;`.
* **Operações de Dispositivo:** Implementa as operações `open`, `release`, `read` e `write` para interação com o dispositivo `/dev/kfetch`.
    * `read`: Retorna um buffer contendo um logotipo personalizado, o nome do host (obrigatório) e as informações do sistema com base na máscara definida.
    * `write`: Permite que um programa do espaço do usuário defina a máscara de informação para futuras leituras.
    * `open`/`release`: Gerenciam proteções para acesso concorrente em ambientes multithreaded.
* **Design Robusto:** As operações `open` e `release` incluem mecanismos de proteção para evitar condições de corrida em ambientes multithread.
* **Limpeza de Recursos:** Garante que todos os recursos alocados (memória, números major/minor do dispositivo) sejam liberados corretamente ao descarregar o módulo.

#### **Como Usar**

Para usar o `kfetch_mod`, siga estes passos:

1.  **Navegue** até o diretório do módulo `kfetch_mod` (`system_info/`).
2.  **Compile o módulo do kernel** e o programa de usuário (`kfetch`):
    ```bash
    make 
    ```
3.  **Compile o programa de nivel de usuário** utilizando o seguinte comando:
    ```bash
    gcc -o kfetch kfetch.c
    ```

4.  **Carregue o módulo** no kernel:
    ```bash
    sudo insmod kfetch_mod.ko
    ```
5.  **Para ler informações** do sistema (todas as informações por padrão, ou as definidas pela máscara):
    ```bash
    sudo ./kfetch 
    ```
6.  **Para escrever uma máscara** de informação (ex: exibir CPU Model e Memory), passando o número da máscara como argumento. Será necessário um programa em C no espaço do usuário para escrever no `/dev/kfetch`. Por exemplo, para `KFETCH_CPU_MODEL | KFETCH_MEM` (que é `(1 << 2) | (1 << 3)` = `4 | 8` = `12`):
    ```bash
    sudo ./kfetch "12"
    ```
    *Obs: O programa `kfetch.c` deve ser capaz de receber um argumento e escrevê-lo para o dispositivo `/dev/kfetch`.*
7.  **Descarregar o Módulo:**
    ```bash
    sudo rmmod kfetch_mod
    ```
8.  **Limpar arquivos gerados:**
    ```bash
    make clean
    ```

---


### 2. Módulo 2: Sistema de Pontuação de Comportamento de Processos

Este módulo de kernel Linux monitora processos em execução e atribui uma nota de risco (Baixo, Médio, Alto) com base no consumo de recursos e comportamento.

## Métricas Monitoradas

O módulo coleta e analisa estas métricas a cada 5 segundos:

| Métrica               | Descrição                                                                 | Unidade  |
|-----------------------|---------------------------------------------------------------------------|----------|
| Uso de CPU            | Tempo de CPU consumido no último intervalo                                | ms       |
| Chamada de Sistema    | Falhas de página (minor + major) como proxy para atividade do sistema     | contagem |
| E/S                   | Bytes lidos/escritos no último intervalo                                  | KB       |
| Memória RSS           | Memória física residente atual                                            | MB       |

## Algoritmo de Avaliação de Risco

Cada métrica contribui para uma pontuação conforme os limiares:

| Métrica               | Limiar Médio         | Limiar Alto          |
|-----------------------|----------------------|----------------------|
| Uso de CPU (ms)       | > 200 ms             | > 800 ms             | 
| Chamada de Sistema    | > 500                | > 3000               |
| E/S (KB)              | > 500 KB             | > 2000 KB            |
| Memória RSS (MB)      | > 350 MB             | > 600 MB             |

### Cálculo da Pontuação:
- **Acima do limiar médio**: +1 ponto por métrica
- **Acima do limiar alto**: +2 pontos por métrica

### Classificação de Risco:
- **Baixo**: 0 pontos
- **Médio**: 1-3 pontos
- **Alto**: ≥4 pontos

#### **Saída dos Resultados:**

Os resultados da avaliação de risco, incluindo a nota atribuída e as métricas relevantes, são exportados para o sistema de arquivos `/proc`. Cada processo terá um arquivo específico dentro de `/proc/process_risk/<pid>`, permitindo fácil visualização com ferramentas padrão como `cat`.

#### **Como Usar**

Para usar o módulo de monitoramento de processos, siga estes passos:

1.  **Navegue** até o diretório do módulo `process_monitor`(`process/`).
2.  **Compile o módulo do kernel**:
    ```bash
    make
    ```
3.  **Carregue o módulo** no kernel:
    ```bash
    sudo insmod process_risk.ko
    ```
4.  **Liste os arquivos** criados no `/proc` para ver os processos monitorados:
    ```bash
    ls /proc/process_risk/
    ```
5.  **Visualize a pontuação de risco** de um processo específico:
    ```bash
    cat /proc/process_risk/<pid>
    while true; do cat /proc/process_risk/<pid>; sleep 5; done               
    ```
    A saída será similar a:
    ```
    PID: <pid>
    Nome: <processo>
    Uso de CPU (delta ms/5s): 20
    Chamadas de Sistema (delta aprox/5s): 39
    E/S Total (delta KB/5s): 0
    Memória (MB): 397
    Risco: Médio
    ```
6.  **Descarregar o Módulo:**
    ```bash
    sudo rmmod process_risk.ko
    ```
7.  **Limpar arquivos gerados:**
    ```bash
    make clean
    ```

---

## Integrantes
Este projeto foi desenvolvido com foco eficaz em equipe, visando alcançar um objetivo complexo de desenvolvimento de software de nível de sistema.

Os integrantes são: 

- [Alexandre Augusto Tescaro Oliveira](https://github.com/alehholiveira)
- [Augusto Guaschi Morato](https://github.com/Guto06)
- [Felipe Dias Konda](https://github.com/FelipeDiasKonda)
- [Hugo Tahara Menegatti](https://github.com/taharaLovelace)
- [Matheus Gonçalves Anitelli](https://github.com/mttue7)
- [Vinícius Barbosa de Souza](https://github.com/ViniBSouza)
- [Vinicius Henrique Galassi](https://github.com/Vgalassi)
