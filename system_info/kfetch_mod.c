#include <linux/atomic.h> 
#include <linux/cdev.h> 
#include <linux/delay.h> 
#include <linux/device.h> 
#include <linux/fs.h> 
#include <linux/init.h> 
#include <linux/kernel.h> 
#include <linux/module.h> 
#include <linux/printk.h> 
#include <linux/types.h> 
#include <linux/uaccess.h> 
#include <linux/version.h> 
#include <linux/sysinfo.h>
#include <asm/errno.h> 
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/sys.h>
#include <linux/utsname.h>
#include <linux/types.h>
#include <linux/smp.h> 
#include <linux/sched/stat.h> 
#include <linux/kernel_stat.h>
#include <linux/sched.h>
#include <linux/mutex.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexandre A., Augusto M., Felipe K., Hugo T., Matheus A., Vinicius B., Vinicius G.");
MODULE_DESCRIPTION("Módulo que cria um dispositivo de caractere que mostra as informações do sistema, como nome do host,
    versão do kernel, modelo da CPU, núcleosonline e total da CPU, memória livre e total, número do processos e uptime");

static DEFINE_MUTEX(info_mutex);


//Definindo máscara 
#define KFETCH_NUM_INFO 6
#define KFETCH_RELEASE   (1 << 0)
#define KFETCH_NUM_CPUS  (1 << 1)
#define KFETCH_CPU_MODEL (1 << 2)
#define KFETCH_MEM       (1 << 3)
#define KFETCH_UPTIME    (1 << 4)
#define KFETCH_NUM_PROCS (1 << 5)

#define KFETCH_FULL_INFO ((1 << KFETCH_NUM_INFO) - 1);

 
//Operaçoes do dispositivo
static int device_open(struct inode *, struct file *); 
static int device_release(struct inode *, struct file *); 
static ssize_t device_read(struct file *, char __user *, size_t, loff_t *); 
static ssize_t device_write(struct file *, const char __user *, size_t, 
                            loff_t *); 
 
//Nome dos dispositivos                          
#define DEVICE_NAME "kfetch" 
//Tamanho da mensagem do dispositivo
#define BUF_LEN 2000 


static int info_mask = KFETCH_FULL_INFO; 

//Numero major do dispositivo
static int major; 
 
enum { 
    CDEV_NOT_USED, 
    CDEV_EXCLUSIVE_OPEN, 
}; 
 
// Previne o acesso de múltiplos dispositivos no device ao mesmo tempo
static atomic_t already_open = ATOMIC_INIT(CDEV_NOT_USED); 
 
//Mensagem quando for solicitada a leitura
static char msg[BUF_LEN + 1]; 
 
static struct class *cls; 

//Operacoes do dispositivo
static struct file_operations kfetch_fops = { 
    .read = device_read, 
    .write = device_write, 
    .open = device_open, 
    .release = device_release, 
}; 
 

/*
Função chamada na inserção do módulo. Responsável por registrar o dispositivo de caractere e criar o dispositivo em /dev.

*/
static int __init kfetch_init(void) 
{ 
    //Criando dispositivo
    major = register_chrdev(0, DEVICE_NAME, &kfetch_fops); 
    if (major < 0) { 
        pr_alert("Registering char device failed with %d\n", major); 
        return major; 
    } 
 
    pr_info("I was assigned major number %d.\n", major); 
    //Criando classe de acordo com a versão do linux (as versões anterior a 6.4.0 apresentam dois parâmetros)
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0) 
        cls = class_create(DEVICE_NAME); 
    #else 
        cls = class_create(THIS_MODULE, DEVICE_NAME); 
    #endif 
        device_create(cls, NULL, MKDEV(major, 0), NULL, DEVICE_NAME); 
    
        pr_info("Device created on /dev/%s\n", DEVICE_NAME); 
    
        return 0; 
} 
/*
Função chamada na remoção do módulo. Realiza a limpeza dos recursos alocados, destruindo o dispositivo e removendo o driver do kernel.
*/
static void __exit kfetch_exit(void) 
{ 
    
    //Destroi o dispositivo que o modulo criou
    device_destroy(cls, MKDEV(major, 0)); 
    class_destroy(cls); 
 
    
    unregister_chrdev(major, DEVICE_NAME); 
} 
 

/*
Executada ao abrir o dispositivo. Coleta dinamicamente as informações do sistema, de acordo com a máscara atual, e as armazena na variável msg. Também aplica um bloqueio com mutex para garantir consistência e evita acessos simultâneos com atomic_t already_open.
*/
static int device_open(struct inode *inode, struct file *file) 
{ 
    //Verifica se arquivo já está sendo usado
    if (atomic_cmpxchg(&already_open, CDEV_NOT_USED, CDEV_EXCLUSIVE_OPEN) != CDEV_NOT_USED) {
        pr_alert("Device already in use\n");
        return -EBUSY; 
    }

    //Linha depois do nome do host
    char *line;

    int j = 0;
    line = kmalloc(256, GFP_KERNEL);
 
    //Calculando o tamanho da linha
    while(utsname()->nodename[j] != '\0'){
        line[j] = '-';
        j++;
    }

    
    //Variaveis para as informacoes do dispositivo
    char release[64] = "";
    char cpu_model[64] = "";
    char mem_info[64] = "";
    char uptime_info[64] = "";
    char procs_info[64] = "";
    char cpus_info[64] = "";

    int local_mask;
    mutex_lock(&info_mutex);
    local_mask = info_mask;
    mutex_unlock(&info_mutex);
    //Informacao versao do kernel
    if (local_mask & KFETCH_RELEASE) {
        snprintf(release, sizeof(release), "Kernel: %s", utsname()->release);
    }

    //Informacao modelo da cpu
    if (local_mask & KFETCH_CPU_MODEL) {
        struct cpuinfo_x86 *c = &cpu_data(0);
        snprintf(cpu_model, sizeof(cpu_model), "CPU: %s", c->x86_model_id);
    }
    //Informacao memoria
    if (local_mask & KFETCH_MEM) {
        struct sysinfo i;
        si_meminfo(&i);
        unsigned long totalram_mb = (i.totalram * i.mem_unit) >> 20;
        unsigned long freeram_mb = (i.freeram * i.mem_unit) >> 20;
        snprintf(mem_info, sizeof(mem_info), "Mem: %lu/%lu MB", freeram_mb, totalram_mb);
    }

    //Informacao uptime
    if (local_mask & KFETCH_UPTIME) {
        struct timespec64 uptime;
        ktime_get_boottime_ts64(&uptime);
        snprintf(uptime_info, sizeof(uptime_info), "Uptime: %llu minutos", uptime.tv_sec / 60);
    }

    //Informacao num de processos
    if (local_mask & KFETCH_NUM_PROCS) {
        struct task_struct *task;
        unsigned int num_procs = 0;
        for_each_process(task)
            num_procs++;
        snprintf(procs_info, sizeof(procs_info), "Procs: %u", num_procs);
    }

    //Informacao num de cpus
    if (local_mask & KFETCH_NUM_CPUS) {
        snprintf(cpus_info, sizeof(cpus_info), "CPUs: %u / %u", num_online_cpus(), num_possible_cpus());
    }
 
   
    // Monta a mensagem com todas as informacoes e um belo javali
    snprintf(msg, BUF_LEN,
    "            ⣦⣼⣷⣦⣄⠀⢠⣶⠀⠀⠀⠀⢀⣠⠆⠀⠀      %s\n"
    "⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⣴⣿⣿⣿⣿⣿⣿⠀⣿⣿⡆⢀⡀⠀⠛⠟⠀⠀⠀     %s\n"
    "⠀⠀⠀⠀⠀⠀⠀⣀⣴⣾⣿⣿⣿⣿⣿⣿⣿⣿⢀⣿⣿⣇⣸⣿⣿⣶⠀⠀⠀⠀     %s\n"
    "⠀⠀⠀⢀⣠⣴⣾⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠀⠀⠀⠀     %s\n"
    "⠀⠀⣶⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡁⠉⢻⣿⣿⣿⠀⠀⠀⠀     %s\n"
    "⠀⢀⣿⣿⣿⣿⣿⣿⣿⣿⣿⡟⢹⣿⣿⣿⣿⠈⣿⣿⣿⣷⣾⣿⣿⣿⡄⠀⠀⠀     %s\n"
    "⠀⢸⣿⣿⣿⡏⣿⣿⣿⣿⣿⡅⠸⣿⣿⣿⣿⡇⠸⣿⣿⣿⣿⡿⠉⢿⣿⣦⡄⠀     %s\n"
    "⠀⢸⣿⣿⣿⠀⢻⣿⣿⣿⣿⣧⠀⢻⣿⣿⣿⣷⠀⠻⣿⣯⣭⣴⠆⢸⡿⠋⠀⠀     %s\n"
    "⠀⠀⠈⢿⣿⡇⠈⢻⣿⣿⣿⣿⣧⡀⠙⢿⣿⣿⡆⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀      \n"
    "⠀⠀⠀⢸⣿⣿⡄⠀⠙⠋⠉⠛⠋⠉⠀⠈⠻⣿⣇⠀⠀⣶⣶⡄⠀⠀⠀⠀⠀⠀\n"
    "⠀⠀⠀⢸⣿⡟⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠹⣿⠀⠀⢸⣿⡇⠀⠀⠀⠀⠀⠀\n"
    "⠀⠀⠀⠛⠛⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠛⠃⠀⠀⠛⠃⠀ \n",
    utsname()->nodename,line,release,cpu_model,cpus_info,mem_info,procs_info,uptime_info);
 

    kfree(line);

    try_module_get(THIS_MODULE); 
    return 0; 
} 
 
/* 
Executada quando o dispositivo é fechado. Libera o controle exclusivo do dispositivo e decrementa o contador de uso do módulo.
*/
static int device_release(struct inode *inode, struct file *file) 
{ 
    //Libera o dispositvo pro proximo usar
    atomic_set(&already_open, CDEV_NOT_USED); 
 
    //Decrementando o contador de uso
    module_put(THIS_MODULE); 
 
    return 0; 
} 
 

/*
Executada ao ler o dispositivo. Copia o conteúdo da variável msg (preenchida em device_open) para o espaço do usuário usando put_user.
*/
static ssize_t device_read(struct file *filp, 
                           char __user *buffer, 
                           size_t length,  
                           loff_t *offset) 
{ 

    int bytes_read = 0; 
    const char *msg_ptr = msg; 
 
    //Se estiver no fim da mensagem
    if (!*(msg_ptr + *offset)) { 
        *offset = 0; 
        return 0; 
    } 
 
    msg_ptr += *offset; 
 
    //Colocando mensagem no buffer
    while (length && *msg_ptr) { 
        put_user(*(msg_ptr++), buffer++); 
        length--; 
        bytes_read++; 
    } 
 
    *offset += bytes_read; 
 
    
    return bytes_read; 
} 
 
/*
device_write: Executada ao escrever no dispositivo. Lê uma string do usuário contendo um número inteiro (representando a nova máscara de bits),
converte-a e atualiza a variável info_mask, que define quais informações serão exibidas nas próximas leituras. Toda a operação é protegida por mutex para garantir exclusividade durante a atualização.
*/
static ssize_t device_write(struct file *filp, const char __user *buff, 
                            size_t len, loff_t *off) 
{ 
    char kbuf[16];
    int mask_info;
    //Verificando tamanho esta correto
    if (len >= sizeof(kbuf)) {
        pr_alert("Too long input\n");
        return -EINVAL;
    }

    //Copiando informacoes de escritas para kbuf e verificando se der algum erro
    if (copy_from_user(kbuf, buff, len))
        return -EFAULT;

    kbuf[len] = '\0'; 
    //Verificando se é possível converter para inteiro
    if (kstrtoint(kbuf, 10, &mask_info) < 0) {
        pr_alert("Error, couldn't covert to int\n");
        return -EINVAL;
    }
    //Atualizando mascara com a nova informacao
    mutex_lock(&info_mutex);
    info_mask = mask_info;
    mutex_unlock(&info_mutex);
    pr_info("Mask updated: %d\n", info_mask);
    return len;
} 
 
module_init(kfetch_init); 
module_exit(kfetch_exit); 
 
