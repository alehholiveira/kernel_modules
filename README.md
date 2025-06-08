# Projeto de Módulos do Kernel Linux

Bem-vindos ao projeto de implementação de módulos do kernel! Este repositório contém dois módulos do kernel Linux desenvolvidos para estender a funcionalidade do sistema operacional em tempo de execução, sem a necessidade de recompilar o kernel completo.

---

## Visão Geral do Projeto

Este projeto visa aprofundar o conhecimento em programação de nível de sistema, especificamente na criação de módulos de kernel para ambientes Ubuntu/Linux. Os módulos foram projetados para demonstrar a capacidade de estender o kernel e interagir com o espaço do usuário de forma controlada e eficiente.

---

## Módulos Implementados

O projeto consiste em dois módulos principais:

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

1.  **Navegue** até o diretório do módulo `kfetch_mod` (`kfetch_mod_dir/`).
2.  **Compile o módulo do kernel** e o programa de usuário (`kfetch`):
    ```bash
    make # Compila kfetch_mod.ko e o programa de usuário kfetch.c
    ```
3.  **Compile o programa de nivel de usuário** utilizando o seguinte comando:
    ```bash
    gcc -o kfetch kfetch.o # Compila o programa
    ```

4.  **Carregue o módulo** no kernel:
    ```bash
    sudo insmod kfetch_mod.ko
    ```
5.  **Para ler informações** do sistema (todas as informações por padrão, ou as definidas pela máscara):
    ```bash
    sudo ./kfetch # Executa o programa user-space para ler do /dev/kfetch
    ```
6.  **Para escrever uma máscara** de informação (ex: exibir CPU Model e Memory), passando o número da máscara como argumento. Será necessário um programa em C no espaço do usuário para escrever no `/dev/kfetch`. Por exemplo, para `KFETCH_CPU_MODEL | KFETCH_MEM` (que é `(1 << 2) | (1 << 3)` = `4 | 8` = `12`):
    ```bash
    sudo ./kfetch "12" # Escreve o número 12 como máscara para o /dev/kfetch
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

Este módulo monitora continuamente métricas chave para cada processo em execução no sistema, como uso da CPU, chamadas de sistema e atividade de E/S. Com base nessas métricas, ele atribui uma **nota de risco** (Baixo, Médio ou Alto) a cada processo.

#### **Algoritmo de Avaliação de Risco (Proposta)**

O algoritmo de avaliação de risco é baseado em uma combinação de limiares e padrões de comportamento:

* **Uso da CPU:**
    * `Baixo`: < 10% de uso médio nos últimos 5 segundos.
    * `Médio`: 10% - 50% de uso médio nos últimos 5 segundos.
    * `Alto`: > 50% de uso médio nos últimos 5 segundos.
* **Chamadas de Sistema (syscalls):**
    * `Baixo`: < 100 syscalls/segundo.
    * `Médio`: 100 - 500 syscalls/segundo.
    * `Alto`: > 500 syscalls/segundo (pode indicar atividade incomum ou maliciosa).
* **Atividade de E/S (leitura/escrita de blocos):**
    * `Baixo`: < 100 blocos/segundo.
    * `Médio`: 100 - 1000 blocos/segundo.
    * `Alto`: > 1000 blocos/segundo (pode indicar acesso excessivo a disco, como ransomware ou mineradores).
* **Critério Combinado:**
    * Se qualquer métrica individual atingir `Alto`, a nota de risco geral é `Alto`.
    * Se duas ou mais métricas individuais atingirem `Médio`, a nota de risco geral é `Médio`.
    * Caso contrário, a nota de risco é `Baixo`.

#### **Saída dos Resultados:**

Os resultados da avaliação de risco, incluindo a nota atribuída e as métricas relevantes, são exportados para o sistema de arquivos `/proc`. Cada processo terá um arquivo específico dentro de `/proc/<pid>/`, como `/proc/<pid>/behavior_score`, permitindo fácil visualização com ferramentas padrão como `cat`.

#### **Como Usar**

Para usar o módulo de monitoramento de processos, siga estes passos:

1.  **Navegue** até o diretório do módulo `process_behavior_mod` (`process_behavior_mod_dir/`).
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
5.  **Visualize a pontuação de risco** de um processo específico ou do resumo geral (dependendo de como você implementou a exposição dos dados no `/proc`):
    ```bash
    cat /proc/process_risk/<PID_DO_PROCESSO> # Para um PID específico (se implementado)
    cat /proc/process_risk                 # Ou para um resumo geral, se houver um único arquivo
    ```
    A saída será similar a:
    ```
    PID: 1234
    Nome: minha_aplicacao
    CPU: 15% (Médio)
    Syscalls: 250/s (Médio)
    I/O: 500 blocks/s (Médio)
    Nota de Risco: Médio
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

## Instruções de Compilação e Carregamento

Para compilar e carregar os módulos em um sistema Ubuntu Linux, siga as instruções abaixo:

### 1. Pré-requisitos

Certifique-se de ter os **cabeçalhos do kernel** instalados. Você pode instalá-los com o seguinte comando:

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

### 2. Compilação

   1. Clone esse repositório ou navegue até o diretório do projeto
   2. Para cada módulo, navegue até seu respectivo diretório e execute make. Exemplo:
   ```bash
   cd kfetch_mod_dir/ # Navegue para o diretório do módulo kfetch_mod
   make               # Compila kfetch_mod.ko e, se houver, o programa de usuário kfetch

   cd ../process_behavior_mod_dir/ # Navegue para o diretório do módulo process_behavior_mod
   make                            # Compila process_risk.ko
   ```
   Isso gerará os arquivos .ko (Kernel Object) e os executáveis de espaço de usuário (se aplicável).

### 3. Carregamento do Módulo
Após a compilação bem-sucedida, você pode carregar o módulo no kernel usando insmod. Lembre-se de que isso requer privilégios de root.

```bash
sudo insmod kfetch_mod.ko         # Para o módulo kfetch_mod
sudo insmod process_risk.ko # Para o módulo process_behavior_mod (nome do arquivo .ko)
```
Você pode verificar se o módulo foi carregado usando lsmod:
```bash
lsmod | grep kfetch_mod
lsmod | grep process_risk
```

### 4. Descarregamento do Módulo
Para remover o módulo do kernel, use rmmod.
```bash
sudo rmmod kfetch_mod         # Para o módulo kfetch_mod
sudo rmmod process_risk.ko # Para o módulo process_behavior_mod
```

### 5. Limpeza
Para remover os arquivos gerados pela compilação (dentro de cada diretório de módulo):
```bash
cd kfetch_mod_dir/
make clean

cd ../process_behavior_mod_dir/
make clean
```

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
