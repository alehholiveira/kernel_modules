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
#define MONITOR_INTERVAL_JIFFIES (5 * HZ)

struct process_risk_info {
    pid_t pid;
    char comm[TASK_COMM_LEN];

    u64 current_utime_ns;
    u64 current_stime_ns;
    u64 current_read_bytes;
    u64 current_write_bytes;
    unsigned long current_min_flt;
    unsigned long current_maj_flt;

    u64 prev_utime_ns;
    u64 prev_stime_ns;
    u64 prev_read_bytes;
    u64 prev_write_bytes;
    unsigned long prev_min_flt;
    unsigned long prev_maj_flt;

    unsigned long cpu_delta_ms;
    unsigned long syscalls_delta;
    unsigned long io_delta_kb;
    unsigned long mem_rss_mb;

    char risk_level[10];
    struct list_head list; 
};

static struct proc_dir_entry *parent_dir;
static struct timer_list monitor_timer;
static LIST_HEAD(process_info_list); 
static DEFINE_MUTEX(process_info_mutex);

#define CPU_DELTA_MEDIUM_THRESHOLD_MS  200
#define CPU_DELTA_HIGH_THRESHOLD_MS    800

#define SYSCALLS_DELTA_MEDIUM_THRESHOLD  500
#define SYSCALLS_DELTA_HIGH_THRESHOLD    3000

#define IO_DELTA_MEDIUM_THRESHOLD_KB   500
#define IO_DELTA_HIGH_THRESHOLD_KB     2000

#define MEM_RSS_MEDIUM_THRESHOLD_MB  350
#define MEM_RSS_HIGH_THRESHOLD_MB    600

#define TOTAL_SCORE_MEDIUM_RISK 1
#define TOTAL_SCORE_HIGH_RISK   4

static void evaluate_and_set_risk(struct process_risk_info *info) {
    int score = 0;

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

    if (score >= TOTAL_SCORE_HIGH_RISK) {
        strncpy(info->risk_level, "Alto", sizeof(info->risk_level) - 1);
    } else if (score >= TOTAL_SCORE_MEDIUM_RISK) {
        strncpy(info->risk_level, "Médio", sizeof(info->risk_level) - 1);
    } else {
        strncpy(info->risk_level, "Baixo", sizeof(info->risk_level) - 1);
    }
    info->risk_level[sizeof(info->risk_level) - 1] = '\0';
}

static int proc_pid_show(struct seq_file *m, void *v) {
    struct process_risk_info *info = (struct process_risk_info *)m->private;

    if (!info) {
        return -ESRCH;
    }

    mutex_lock(&process_info_mutex);

    seq_printf(m,
        "PID: %d\n"
        "Nome: %s\n"
        "Uso de CPU (delta ms/5s): %lu\n"
        "Chamadas de Sistema (delta aprox/5s): %lu\n"
        "E/S Total (delta KB/5s): %lu\n"
        "Memória (MB): %lu\n"
        "Risco: %s\n",
        info->pid,
        info->comm,
        info->cpu_delta_ms,
        info->syscalls_delta,
        info->io_delta_kb,
        info->mem_rss_mb,
        info->risk_level
    );

    mutex_unlock(&process_info_mutex);

    return 0;
}

static int proc_pid_open(struct inode *inode, struct file *file) {
    struct process_risk_info *info = pde_data(inode);
    return single_open(file, proc_pid_show, info);
}

static const struct proc_ops pid_file_ops = {
    .proc_open    = proc_pid_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};


static void monitor_processes_callback(struct timer_list *t) {
    struct task_struct *task;
    struct process_risk_info *info, *temp;
    LIST_HEAD(existing_process_list_snapshot);

    mutex_lock(&process_info_mutex);

    list_splice_init(&process_info_list, &existing_process_list_snapshot);

    rcu_read_lock();
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

                info->mem_rss_mb = 0;
                if (task->mm) {
                    info->mem_rss_mb = (get_mm_rss(task->mm) * PAGE_SIZE) >> 20;
                }
                
                evaluate_and_set_risk(info);

                list_move_tail(&info->list, &process_info_list); 
                break;
            }
        }

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
    rcu_read_unlock();

    list_for_each_entry_safe(info, temp, &existing_process_list_snapshot, list) {
        pr_info("Processo %d (%s) terminado. Removendo entrada /proc.\n", info->pid, info->comm);
        char filename[16];
        snprintf(filename, sizeof(filename), "%d", info->pid);
        remove_proc_entry(filename, parent_dir);
        list_del(&info->list);
        kfree(info);
    }

    mutex_unlock(&process_info_mutex);

    mod_timer(&monitor_timer, jiffies + MONITOR_INTERVAL_JIFFIES);
}


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
