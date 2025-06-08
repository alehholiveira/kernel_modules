#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define DEVICE_PATH "/dev/kfetch"

int main(int argc, char *argv[]) {
    int fd;

    if (argc == 2) {
        int mask = atoi(argv[1]);

        fd = open(DEVICE_PATH, O_WRONLY);
        if (fd < 0) {
            perror("Erro ao abrir o dispositivo para escrita");
            return 1;
        }

        char buf[16];
        snprintf(buf, sizeof(buf), "%d", mask);

        ssize_t written = write(fd, buf, strlen(buf));
        if (written < 0) {
            perror("Erro ao escrever no dispositivo");
            close(fd);
            return 1;
        }

        printf("Máscara %d escrita com sucesso no dispositivo.\n", mask);
        close(fd);
    } else {
      
        fd = open(DEVICE_PATH, O_RDONLY);
        if (fd < 0) {
            perror("Erro ao abrir o dispositivo para leitura");
            return 1;
        }
        char buf[2048]; 
        ssize_t bytesRead = read(fd, buf, sizeof(buf) - 1);
        if (bytesRead < 0) {
            perror("Erro ao ler do dispositivo");
            close(fd);
            return 1;
        }

        buf[bytesRead] = '\0';
        printf("%s\n", buf);
        close(fd);
    }

    return 0;
}
