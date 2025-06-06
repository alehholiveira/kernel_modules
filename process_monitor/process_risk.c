#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched/signal.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

// Resumo da Confiabilidade das Métricas: (por enquanto)

    // CPU: Ótimo.
    // Chamadas de Sistema (estimativa): Pode ser um ponto fraco se a precisão for a principal preocupação. É mais uma métrica de "atividade de paginação".
    // E/S: Ótimo.
    // Memória: Ótimo (especialmente com RSS).

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexandre A., Augusto M., Felipe K., Hugo T., Matheus A., Vinicius B., Vinicius G.");
MODULE_DESCRIPTION("Módulo que cria arquivos /proc/process_risk/[pid] com avaliação de risco");

#define PROC_DIRNAME "process_risk"

static struct proc_dir_entry *parent_dir;

// Função chamada para imprimir o conteúdo de cada arquivo /proc/process_risk/<pid>
static int proc_pid_show(struct seq_file *m, void *v) {
    struct task_struct *task = m->private;

    if (!task)
        return -ESRCH;

    unsigned long utime = task->utime;
    unsigned long stime = task->stime;
    unsigned long cpu_time_ms = ((utime + stime) * 1000) / HZ;

    // Estimativa grosseira de chamadas de sistema
    unsigned long syscalls_est = task->min_flt + task->maj_flt;

    // E/S em KB (talvez troque para mb)
    unsigned long long io_bytes = 0;
    unsigned long io_kb = 0;
    if (task->ioac.read_bytes || task->ioac.write_bytes)
        io_bytes = task->ioac.read_bytes + task->ioac.write_bytes;
    io_kb = io_bytes >> 10;  // bytes / 1024

    // Memória residente em MB (RSS)
    unsigned long mem_rss_pages = 0;
    unsigned long mem_mb = 0;
    if (task->mm) {
        mem_rss_pages = get_mm_rss(task->mm);  // em páginas
        mem_mb = (mem_rss_pages * PAGE_SIZE) >> 20; // -> MB
    }

    // Avaliação de risco (foi escolhida uma abordagem de score (detalhar mais))
    char *risk_level = "Baixo";
    int score = 0;
    if (cpu_time_ms > 5000)     score++;
    if (syscalls_est > 5000)    score++;
    if (io_kb > 10000)          score++;
    if (mem_mb > 200)           score++;
    if (score >= 3)
        risk_level = "Alto";
    else if (score == 2)
        risk_level = "Médio";

    seq_printf(m,
        "PID: %d\n"
        "Nome: %s\n"
        "Uso de CPU (ms): %lu\n"
        "Chamadas de Sistema (aprox): %lu\n"
        "E/S Total (KB): %lu\n"
        "Memória (MB): %lu\n"
        "Risco: %s\n",
        task->pid,
        task->comm,
        cpu_time_ms,
        syscalls_est,
        io_kb,
        mem_mb,
        risk_level
    );

    return 0;
}

static int proc_pid_open(struct inode *inode, struct file *file) {
    struct task_struct *task = pde_data(inode);
    return single_open(file, proc_pid_show, task);
}

static const struct proc_ops pid_file_ops = {
    .proc_open    = proc_pid_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

// Cria um arquivo /proc/process_risk/<pid> para um processo específico
static void create_proc_entry_for_task(struct task_struct *task) {
    char filename[16];
    snprintf(filename, sizeof(filename), "%d", task->pid);
    proc_create_data(filename, 0444, parent_dir, &pid_file_ops, task);
}

static int __init process_risk_init(void) {
    struct task_struct *task;

    parent_dir = proc_mkdir(PROC_DIRNAME, NULL);
    if (!parent_dir) {
        pr_err("Falha ao criar /proc/%s\n", PROC_DIRNAME);
        return -ENOMEM;
    }

    for_each_process(task) {
        get_task_struct(task);  // Protege task até descarregar
        create_proc_entry_for_task(task);
    }

    pr_info("Módulo process_risk carregado.\n");
    return 0;
}

static void __exit process_risk_exit(void) {
    struct task_struct *task;
    char filename[16];

    for_each_process(task) {
        snprintf(filename, sizeof(filename), "%d", task->pid);
        remove_proc_entry(filename, parent_dir);
        put_task_struct(task);
    }

    remove_proc_entry(PROC_DIRNAME, NULL);
    pr_info("Módulo process_risk descarregado.\n");
}

module_init(process_risk_init);
module_exit(process_risk_exit);
