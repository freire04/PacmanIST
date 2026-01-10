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
#include <signal.h>

#define MAX_BOARD_CELLS 1000000

// flag global para determinar se o cliente para ou conitnua a correr
static bool stop_execution = false;

// tempo (ms) entre comandos quando se joga a partir de ficheiro
static int tempo = 500;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// buffer onde o servidor escreve o estado do tabuleiro 
static char *tabuleiro = NULL;

/**
 * receiver_thread:
 * thread que fica bloqueada a receber updates do servidor pelo FIFO de notificações.
 *  1) receive_board_updates(board) le o update e copia o grid para board
 *  2) get_last_board_meta() devolve os dados associados ao ultimo update recebido
 *  3) desenha o tabuleiro via ncurses (draw_board_client + refresh_screen).
 */
static void *receiver_thread(void *arg) {
    (void)arg;

    while (1) {

        // bloqueia ate receber um novo update do servidor
        if (receive_board_updates(tabuleiro) < 0){
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        // le os dados do ultimo update
        Board meta = get_last_board_meta();

        // se o jogo terminou, sinaliza paragem
        if(meta.victory || meta.game_over){
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

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

/**
 * main:
 * thread principal do cliente.
 */
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

    // Se houver ficheiro de comandos, abre para leitura
    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    // evita terminar o processo ao escrever num pipe fechado (SIGPIPE)
    signal(SIGPIPE, SIG_IGN);

    // construcao dos paths para os fifos
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    /* Inicializa debug para um ficheiro local do cliente. */
    open_debug_file("client-debug.log");

    // removes fifos antigos (de execs anteriores)
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
    bool disconnected;

    while (1) {
        pthread_mutex_lock(&mutex);
        bool stop = stop_execution;
        pthread_mutex_unlock(&mutex);
        if(stop)
            break;
        if (cmd_fp) {
            ch = fgetc(cmd_fp);

            if (ch == EOF) {
                rewind(cmd_fp);
                continue;
            }

            command = (char)ch;

            if (command == '\n' || command == '\r' || command == '\0')
                continue;
            command = (char)toupper((unsigned char) command);
            
            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            pthread_mutex_unlock(&mutex);

            sleep_ms(wait_for);
            
        } else {
            command = get_input();
            command = (char)toupper((unsigned char)command);
        }

        if (command == '\0')
            continue;

        // input 'Q' termina o cliente e notifica o servidor
        if (command == 'Q') {
            debug("Client pressed 'Q', quitting game\n");
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);

            disconnected = true;
            pacman_disconnect();
            break;
        }

        debug("Command: %c\n", command);

        if(pacman_play(command) < 0){
            debug("pacman_play failed\n");
            break;
        }

    }

    pthread_mutex_lock(&mutex);
    stop_execution = true;
    pthread_mutex_unlock(&mutex);

    pthread_join(receiver_thread_id, NULL);

    if(!disconnected) pacman_disconnect();

    if (cmd_fp)
        fclose(cmd_fp);

    free(tabuleiro);
    pthread_mutex_destroy(&mutex);

    terminal_cleanup();
    fflush(stdout);

    return 0;
}
