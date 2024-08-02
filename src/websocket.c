#include "websocket.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

void write_message(config_t *config, char *msg, int msg_len) {
    int fd = open(config->ws_pipein, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "Error opening pipe\n");
        return;
    }
    write(fd, msg, msg_len);
    close(fd);
}
