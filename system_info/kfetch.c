#include <stdio.h>      
#include <stdlib.h>    
#include <fcntl.h>      
#include <unistd.h>     
#include <string.h>   
#include <errno.h>      

#define DEVICE_PATH "/dev/kfetch"  // Caminho do dispositivo criado pelo módulo do kernel

int main(int argc, char *argv[]) {
    int fd;

    // Caso o programa seja chamado com 1 argumento (além do nome), escreve a máscara
    if (argc == 2) {
        int mask = atoi(argv[1]); // Converte a string do argumento para inteiro

        // Abre o dispositivo para escrita
        fd = open(DEVICE_PATH, O_WRONLY);
        if (fd < 0) {
            perror("Erro ao abrir o dispositivo para escrita");
            return 1;
        }

        // Prepara o buffer com a máscara a ser enviada
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", mask);

        // Escreve a máscara no dispositivo
        ssize_t written = write(fd, buf, strlen(buf));
        if (written < 0) {
            perror("Erro ao escrever no dispositivo");
            close(fd);
            return 1;
        }

        printf("Máscara %d escrita com sucesso no dispositivo.\n", mask);
        close(fd); // Fecha o descritor de arquivo
    } else {
        // Se nenhum argumento for fornecido, apenas lê as informações

        fd = open(DEVICE_PATH, O_RDONLY); // Abre o dispositivo para leitura
        if (fd < 0) {
            perror("Erro ao abrir o dispositivo para leitura");
            return 1;
        }

        char buf[2048]; // Buffer grande o suficiente para conter a saída completa
        ssize_t bytesRead = read(fd, buf, sizeof(buf) - 1); // Lê a saída do dispositivo
        if (bytesRead < 0) {
            perror("Erro ao ler do dispositivo");
            close(fd);
            return 1;
        }

        buf[bytesRead] = '\0'; // Garante terminação nula da string lida
        printf("%s\n", buf);    // Imprime a saída formatada do dispositivo
        close(fd);              // Fecha o descritor de arquivo
    }

    return 0; // Retorno bem-sucedido
}
