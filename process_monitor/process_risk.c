#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched/signal.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/jiffies.h>
#include <linux/timer.h> 
#include <linux/list.h>
#include <linux/mutex.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexandre A., Augusto M., Felipe K., Hugo T., Matheus A., Vinicius B., Vinicius G.");
MODULE_DESCRIPTION("Módulo que monitora continuamente processos e avalia risco");

#define PROC_DIRNAME "process_risk"
#define MONITOR_INTERVAL_JIFFIES (5 * HZ) // intervalo de monitoramento (5 segundos)

struct process_risk_info {
    pid_t pid;      
    char comm[TASK_COMM_LEN];            // nome do processo

    // métricas cumulativas brutas do kernel (ex: tempo de CPU em nano segundos)
    u64 current_utime_ns;
    u64 current_stime_ns;
    u64 current_read_bytes;
    u64 current_write_bytes;
    unsigned long current_min_flt;
    unsigned long current_maj_flt;

    // métricas cumulativas da leitura anterior (para cálculo de deltas)
    u64 prev_utime_ns;
    u64 prev_stime_ns;
    u64 prev_read_bytes;
    u64 prev_write_bytes;
    unsigned long prev_min_flt;
    unsigned long prev_maj_flt;

    // métricas calculadas como deltas (uso no último intervalo de 5s)
    unsigned long cpu_delta_ms;         // uso de CPU em millissegundos
    unsigned long syscalls_delta;       // estimativa de chamadadas de sistema com base em page/faults
                                        // (não é uma contagem exata, mas uma aproximação)
    unsigned long io_delta_kb;          // E/S total em KB no intervalo
    unsigned long mem_rss_mb;           // memória fisica RSS em MB (é um valor instantâneo, não um delta)

    char risk_level[10];                // variável para armazenar o nível de risco do processo
                                        // "Baixo", "Médio" ou "Alto"
    struct list_head list;              // nó para a lista encadeada do kernel
};

// variáveis globais para o diretório /proc, timer, lista de processos e mutex
static struct proc_dir_entry *parent_dir;
static struct timer_list monitor_timer;
static LIST_HEAD(process_info_list); 
static DEFINE_MUTEX(process_info_mutex);

// definições dos limiares para a avaliação de risco (valores para deltas e RSS)
#define CPU_DELTA_MEDIUM_THRESHOLD_MS  200
#define CPU_DELTA_HIGH_THRESHOLD_MS    800

#define SYSCALLS_DELTA_MEDIUM_THRESHOLD  500
#define SYSCALLS_DELTA_HIGH_THRESHOLD    3000

#define IO_DELTA_MEDIUM_THRESHOLD_KB   500
#define IO_DELTA_HIGH_THRESHOLD_KB     2000

#define MEM_RSS_MEDIUM_THRESHOLD_MB  350
#define MEM_RSS_HIGH_THRESHOLD_MB    600

#define TOTAL_SCORE_MEDIUM_RISK 1  // pontuação mínima para risco médio
#define TOTAL_SCORE_HIGH_RISK   4  // pontuação mínima para risco alto

static void evaluate_and_set_risk(struct process_risk_info *info) {
    int score = 0;

    // pontua o risco com base nas métricas delta de CPU, chamadas de sistema e E/S
    // e no valor absoluto de memória RSS.
    if (info->cpu_delta_ms > CPU_DELTA_HIGH_THRESHOLD_MS) {
        score += 2; // Alta pontuação
    } else if (info->cpu_delta_ms > CPU_DELTA_MEDIUM_THRESHOLD_MS) {
        score += 1; // Média pontuação
    }

    if (info->syscalls_delta > SYSCALLS_DELTA_HIGH_THRESHOLD) {
        score += 2;
    } else if (info->syscalls_delta > SYSCALLS_DELTA_MEDIUM_THRESHOLD) {
        score += 1;
    }

    if (info->io_delta_kb > IO_DELTA_HIGH_THRESHOLD_KB) {
        score += 2;
    } else if (info->io_delta_kb > IO_DELTA_MEDIUM_THRESHOLD_KB) {
        score += 1;
    }

    if (info->mem_rss_mb > MEM_RSS_HIGH_THRESHOLD_MB) {
        score += 2;
    } else if (info->mem_rss_mb > MEM_RSS_MEDIUM_THRESHOLD_MB) {
        score += 1;
    }

    // define o nível de risco com base na pontuação total
    if (score >= TOTAL_SCORE_HIGH_RISK) {
        strncpy(info->risk_level, "Alto", sizeof(info->risk_level) - 1);
    } else if (score >= TOTAL_SCORE_MEDIUM_RISK) {
        strncpy(info->risk_level, "Médio", sizeof(info->risk_level) - 1);
    } else {
        strncpy(info->risk_level, "Baixo", sizeof(info->risk_level) - 1);
    }
    info->risk_level[sizeof(info->risk_level) - 1] = '\0';
}

// função de callback para leitura do arquivo /proc/process_risk/<pid>
static int proc_pid_show(struct seq_file *m, void *v) {
    struct process_risk_info *info = (struct process_risk_info *)m->private;

    if (!info) {
        return -ESRCH;  // garantindo que a informação nao é invalida
    }

    mutex_lock(&process_info_mutex);    // bloqueia o mutex para acesso seguro à estrutura de dados

    // exibe as informações do processo e o risco atribuido formatadas no arquivo em /proc/process_risk/<pid>
    seq_printf(m,
        "PID: %d\n"
        "Nome: %s\n"
        "Uso de CPU (delta ms/5s): %lu\n"
        "Chamadas de Sistema (delta aprox/5s): %lu\n"
        "E/S Total (delta KB/5s): %lu\n"
        "Memória (MB): %lu\n"
        "Risco: %s\n----------------------------------------------------------------\n",
        info->pid,
        info->comm,
        info->cpu_delta_ms,
        info->syscalls_delta,
        info->io_delta_kb,
        info->mem_rss_mb,
        info->risk_level
    );

    mutex_unlock(&process_info_mutex);  // libera o mutex após a leitura

    return 0;
}

// função de abertura para o arquivo /proc/process_risk/<pid>
static int proc_pid_open(struct inode *inode, struct file *file) {
    struct process_risk_info *info = pde_data(inode);
    return single_open(file, proc_pid_show, info);
}

// definição das operações do arquivo para as entradas /proc/process_risk/<pid>
static const struct proc_ops pid_file_ops = {
    .proc_open    = proc_pid_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

// função de callback do timer: executa periodicamente para monitorar e atualizar processos
static void monitor_processes_callback(struct timer_list *t) {
    struct task_struct *task;
    struct process_risk_info *info, *temp;
    LIST_HEAD(existing_process_list_snapshot);

    mutex_lock(&process_info_mutex); // novamente um mutex para modifiar a lista principal de processos existentes

    list_splice_init(&process_info_list, &existing_process_list_snapshot);

    rcu_read_lock(); // bloqueia a leitura RCU para garantir que a lista de processos não seja modificada enquanto iteramos
    // percorre todos os processos do sistema para iterar para calculo de deltas
    for_each_process(task) {
        bool found = false;
        list_for_each_entry_safe(info, temp, &existing_process_list_snapshot, list) {
            if (info->pid == task->pid) {
                found = true;
                
                info->prev_utime_ns = info->current_utime_ns;
                info->prev_stime_ns = info->current_stime_ns;
                info->prev_read_bytes = info->current_read_bytes;
                info->prev_write_bytes = info->current_write_bytes;
                info->prev_min_flt = info->current_min_flt;
                info->prev_maj_flt = info->current_maj_flt;

                info->current_utime_ns = task->utime;
                info->current_stime_ns = task->stime;
                info->current_read_bytes = task->ioac.read_bytes;
                info->current_write_bytes = task->ioac.write_bytes;
                info->current_min_flt = task->min_flt;
                info->current_maj_flt = task->maj_flt;
                
                u64 total_current_cpu_ns = info->current_utime_ns + info->current_stime_ns;
                u64 total_prev_cpu_ns = info->prev_utime_ns + info->prev_stime_ns;
                u64 cpu_delta_ns = 0;
                if (total_current_cpu_ns > total_prev_cpu_ns) {
                    cpu_delta_ns = total_current_cpu_ns - total_prev_cpu_ns;
                }
                info->cpu_delta_ms = (unsigned long)(cpu_delta_ns / 1000000ULL);

                info->syscalls_delta = (info->current_min_flt + info->current_maj_flt) -
                                       (info->prev_min_flt + info->prev_maj_flt);
                info->io_delta_kb = ((info->current_read_bytes + info->current_write_bytes) -
                                     (info->prev_read_bytes + info->prev_write_bytes)) >> 10;
                
                // coleta o valor da memoria RSS em MB (valor instantâneo)
                info->mem_rss_mb = 0;
                if (task->mm) {
                    info->mem_rss_mb = (get_mm_rss(task->mm) * PAGE_SIZE) >> 20;
                }
                
                evaluate_and_set_risk(info);

                list_move_tail(&info->list, &process_info_list); 
                break;
            }
        }

        // se o processo não foi encontrado na lista, cria o mesmo e coleta as informações
        if (!found) {
            struct process_risk_info *new_info = kmalloc(sizeof(*new_info), GFP_ATOMIC);
            if (new_info) {
                new_info->pid = task->pid;
                strncpy(new_info->comm, task->comm, TASK_COMM_LEN - 1);
                new_info->comm[TASK_COMM_LEN - 1] = '\0';

                new_info->current_utime_ns = task->utime;
                new_info->current_stime_ns = task->stime;
                new_info->current_read_bytes = task->ioac.read_bytes;
                new_info->current_write_bytes = task->ioac.write_bytes;
                new_info->current_min_flt = task->min_flt;
                new_info->current_maj_flt = task->maj_flt;

                new_info->prev_utime_ns = new_info->current_utime_ns;
                new_info->prev_stime_ns = new_info->current_stime_ns;
                new_info->prev_read_bytes = new_info->current_read_bytes;
                new_info->prev_write_bytes = new_info->current_write_bytes;
                new_info->prev_min_flt = new_info->current_min_flt;
                new_info->prev_maj_flt = new_info->current_maj_flt;
                
                new_info->cpu_delta_ms = 0;
                new_info->syscalls_delta = 0;
                new_info->io_delta_kb = 0;

                new_info->mem_rss_mb = 0;
                if (task->mm) {
                    new_info->mem_rss_mb = (get_mm_rss(task->mm) * PAGE_SIZE) >> 20;
                }
                
                evaluate_and_set_risk(new_info);

                list_add_tail(&new_info->list, &process_info_list);
                
                char filename[16];
                snprintf(filename, sizeof(filename), "%d", new_info->pid);
                if (!proc_create_data(filename, 0444, parent_dir, &pid_file_ops, new_info)) {
                     pr_warn("Falha ao criar /proc/%s/%d para novo processo. Removendo da lista.\n", PROC_DIRNAME, new_info->pid);
                     list_del(&new_info->list);
                     kfree(new_info);
                }
            } else {
                pr_err("Falha ao alocar memória para novo processo.\n");
            }
        }
    }
    rcu_read_unlock();  // libera a leitura RCU após iterar por todos os processos

    // remove os processos que não estão mais ativos e libera a memória
    list_for_each_entry_safe(info, temp, &existing_process_list_snapshot, list) {
        pr_info("Processo %d (%s) terminado. Removendo entrada /proc.\n", info->pid, info->comm);
        char filename[16];
        snprintf(filename, sizeof(filename), "%d", info->pid);
        remove_proc_entry(filename, parent_dir);
        list_del(&info->list);
        kfree(info);
    }

    mutex_unlock(&process_info_mutex); // libera o mutex 

    mod_timer(&monitor_timer, jiffies + MONITOR_INTERVAL_JIFFIES); // reinicia o timer para o próximo monitoramento
}

// função de inicialização do módulo: cria o diretório /proc/process_risk e inicia o timer
static int __init process_risk_init(void) {
    pr_info("Iniciando módulo process_risk_monitor...\n");

    parent_dir = proc_mkdir(PROC_DIRNAME, NULL);
    if (!parent_dir) {
        pr_err("Falha ao criar /proc/%s\n", PROC_DIRNAME);
        return -ENOMEM;
    }

    timer_setup(&monitor_timer, monitor_processes_callback, 0);
    mod_timer(&monitor_timer, jiffies + HZ);

    pr_info("Módulo process_risk_monitor carregado e monitoramento iniciado.\n");
    return 0;
}

// função de limpeza do módulo: remove o diretório /proc/process_risk e libera a memória
// além de parar o timer e remover as entradas de processos monitorados
// e liberar a memória alocada para cada entrada de processo
static void __exit process_risk_exit(void) {
    struct process_risk_info *info, *temp;

    pr_info("Descarregando módulo process_risk_monitor...\n");

    del_timer_sync(&monitor_timer);

    mutex_lock(&process_info_mutex);

    list_for_each_entry_safe(info, temp, &process_info_list, list) {
        pr_info("Removendo entrada /proc/%s/%d\n", PROC_DIRNAME, info->pid);
        char filename[16];
        snprintf(filename, sizeof(filename), "%d", info->pid);
        remove_proc_entry(filename, parent_dir);
        list_del(&info->list);
        kfree(info);
    }

    mutex_unlock(&process_info_mutex);

    remove_proc_entry(PROC_DIRNAME, NULL);

    pr_info("Módulo process_risk_monitor descarregado.\n");
}

module_init(process_risk_init);
module_exit(process_risk_exit);
