#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

#define MAX_BOARD_CELLS 1000000

static bool stop_execution = false;
static int tempo = 500;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static char *tabuleiro = NULL;

static void *receiver_thread(void *arg) {
    (void)arg;

    while (1) {

        if (receive_board_updates(tabuleiro) < 0){
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        Board meta = get_last_board_meta();

        size_t cells = (size_t) meta.width * (size_t) meta.height;
        if (meta.width <= 0 || meta.height <= 0 || cells > MAX_BOARD_CELLS) {
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        pthread_mutex_lock(&mutex);
        tempo = meta.tempo;
        if(meta.game_over == 1 || meta.victory == 1)
            stop_execution = true;
        pthread_mutex_unlock(&mutex);

        Board draw = meta;
        draw.data = tabuleiro;

        draw_board_client(draw);
        refresh_screen();

        if (meta.game_over == 1 || meta.victory == 1)
            break;
    }

    debug("Returning receiver thread...\n");
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr,
            "Usage: %s <client_id> <register_pipe> [commands_file]\n",
            argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");

    unlink(req_pipe_path);
    unlink(notif_pipe_path);

    if (mkfifo(req_pipe_path, 0666)< 0) {
        perror("Failed to create request pipe");
        return 1;
    }
    if (mkfifo(notif_pipe_path, 0666) < 0) {
        perror("Failed to create notification pipe");
        unlink(req_pipe_path);
        return 1;
    }

    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0) {
        perror("Failed to connect to server");
        unlink(req_pipe_path);
        unlink(notif_pipe_path);
        return 1;
    }

    tabuleiro = malloc(MAX_BOARD_CELLS);
    if(!tabuleiro){
        perror("malloc tabuleiro");
        pacman_disconnect();
        return 1;
    }

    terminal_init();
    set_timeout(500);
    clear();
    mvprintw(0, 0, "A ligar ao servidor...");
    refresh_screen();

    pthread_t receiver_thread_id;
    if(pthread_create(&receiver_thread_id, NULL, receiver_thread, NULL) != 0){
        perror("pthread_create");
        pacman_disconnect();
        free(tabuleiro);
        terminal_cleanup();
        return 1;
    }

    char command;
    int ch;

    while (1) {

        pthread_mutex_lock(&mutex);
        bool stop = stop_execution;
        pthread_mutex_unlock(&mutex);
        if(stop)
            break;

        if (cmd_fp) {
            // Input from file
            ch = fgetc(cmd_fp);

            if (ch == EOF) {
                // Restart at the start of the file
                rewind(cmd_fp);
                continue;
            }

            command = (char)ch;

            if (command == '\n' || command == '\r' || command == '\0')
                continue;

            command = (char)toupper((unsigned char) command);
            
            // Wait for tempo, to not overflow pipe with requests
            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            pthread_mutex_unlock(&mutex);

            sleep_ms(wait_for);
            
        } else {
            // Interactive input
            command = get_input();
            command = (char)toupper((unsigned char)command);
        }

        if (command == '\0')
            continue;

        if (command == 'Q') {
            debug("Client pressed 'Q', quitting game\n");
            break;
        }

        debug("Command: %c\n", command);

        if(pacman_play(command) < 0){
            debug("pacman_play failed\n");
            break;
        }

    }

    pacman_disconnect();

    pthread_join(receiver_thread_id, NULL);

    if (cmd_fp)
        fclose(cmd_fp);

    free(tabuleiro);
    pthread_mutex_destroy(&mutex);

    terminal_cleanup();

    return 0;
}
