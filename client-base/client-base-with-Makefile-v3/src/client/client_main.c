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

// flag global de paragem, protegida por mutex
static bool stop_execution = false;

// tempo entre updates, vindo do server
static int tempo = 500;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// buffer do tabuleiro preenchido por receive_board_updates (api) e passado por draw_board_client
static char *tabuleiro = NULL;

/**
 * receiver_thread:
 * thread dedicada a receber updates do servidor através do FIFO de notificações.
 *  1) bloqueia/recebe uma atualização via receive_board_updates(tabuleiro)
 *  2) obtem metadados do ultimo update (width/height/tempo/victory/game_over/points)
 *  3) redraw o board no cliente via ncurses
 *
 * termina quando:
 *  A) o servidor fecha o pipe / ocorre erro em receive_board_updates()
 *  B) victory ou game_over chegam a 1
 */
static void *receiver_thread(void *arg) {
    (void)arg;

    while (1) {
        // recebe novo estado do jogo
        if (receive_board_updates(tabuleiro) < 0){
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        // metadados associados ao ultimo update recebido
        Board meta = get_last_board_meta();
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

        // prepara uma board para desenhar
        Board draw = meta;
        draw.data = tabuleiro;

        draw_board_client(draw);
        refresh_screen();

        if (meta.game_over == 1 || meta.victory == 1)
            break;
    }

    return NULL;
}

/**
 * main:
 * aka thread principal do cliente
 *  1) interpretar argumentos 
 *  2) Criar fifos do cliente (request + notification)
 *  3) estabelecer sessao com o servidor (pacman_connect)
 *  4) inicializar ncurses e lançar receiver_thread
 *  5) ler comandos (stdin ou ficheiro) e enviar para o servidor (pacman_play)
 *  6) fazer cleanup: join, pacman_disconnect, fechar ficheiros, free, terminal_cleanup
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

    // se houver ficheiro, abre para leitura
    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    // evita que writes para pipes fechados terminem para o cliente via SIGPIPE 
    signal(SIGPIPE, SIG_IGN);

    // caminhos para os fifos
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");

    // remove fifos antigos (de execs anteriores)
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
