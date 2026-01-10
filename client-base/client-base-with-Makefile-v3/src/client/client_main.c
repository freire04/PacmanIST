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

/* Limite máximo de células do tabuleiro para evitar alocações/leituras absurdas
 * (protege contra valores inválidos vindos do servidor). */
#define MAX_BOARD_CELLS 1000000

/* Flag global para terminar o cliente (lida/escrita por 2 threads).
 * Deve ser acedida sempre com mutex. */
static bool stop_execution = false;

/* Tempo (ms) entre comandos quando se joga a partir de ficheiro.
 * É atualizado com o valor recebido do servidor. */
static int tempo = 500;

/* Mutex para proteger stop_execution e tempo. */
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* Buffer onde o servidor escreve o estado do tabuleiro (grid linear).
 * É alocado no main e preenchido na receiver_thread. */
static char *tabuleiro = NULL;

/**
 * receiver_thread:
 * Thread que fica bloqueada a receber updates do servidor pelo FIFO de notificações.
 *
 * Fluxo:
 *  1) receive_board_updates(tabuleiro) lê um update (meta + grid) e copia o grid para tabuleiro.
 *  2) get_last_board_meta() devolve os metadados associados ao último update recebido.
 *  3) valida width/height e atualiza o "tempo" partilhado.
 *  4) desenha o tabuleiro via ncurses (draw_board_client + refresh_screen).
 *
 * Termina quando:
 *  - receive_board_updates falha (servidor fechou o pipe / erro)
 *  - victory ou game_over chegam a 1
 *  - metadados inválidos (dimensões incoerentes ou demasiado grandes)
 */
static void *receiver_thread(void *arg) {
    (void)arg;

    while (1) {

        /* Bloqueia até receber um novo update do servidor.
         * Em erro/EOF, marca stop_execution para parar o main. */
        if (receive_board_updates(tabuleiro) < 0){
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        /* Lê metadados do último update. */
        Board meta = get_last_board_meta();

        /* Se o jogo terminou (vitória/derrota), sinaliza paragem. */
        if(meta.victory || meta.game_over){
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        /* Validação defensiva para evitar desenhar/aceder fora do buffer. */
        size_t cells = (size_t) meta.width * (size_t) meta.height;
        if (meta.width <= 0 || meta.height <= 0 || cells > MAX_BOARD_CELLS) {
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        /* Atualiza o tempo partilhado (usado para “rate-limit” de comandos via ficheiro). */
        pthread_mutex_lock(&mutex);
        tempo = meta.tempo;
        if(meta.game_over == 1 || meta.victory == 1)
            stop_execution = true;
        pthread_mutex_unlock(&mutex);

        /* Prepara uma estrutura Board para desenhar: meta + ponteiro para o grid. */
        Board draw = meta;
        draw.data = tabuleiro;

        /* Desenha e refresca o ecrã. */
        draw_board_client(draw);
        refresh_screen();

        /* Redundante mas explícito: se acabou, sai. */
        if (meta.game_over == 1 || meta.victory == 1)
            break;
    }

    debug("Returning receiver thread...\n");
    return NULL;
}

/**
 * main:
 * Thread principal do cliente.
 *
 * Responsabilidades:
 *  1) Validar argumentos: <client_id> <register_pipe> [commands_file]
 *  2) (Opcional) abrir ficheiro de comandos
 *  3) Criar FIFOs do cliente: /tmp/<id>_request e /tmp/<id>_notification
 *  4) Ligar ao servidor: pacman_connect(req, notif, register_pipe)
 *  5) Alocar buffer do tabuleiro e inicializar ncurses
 *  6) Lançar receiver_thread (recebe e desenha updates)
 *  7) Ler comandos (ficheiro ou teclado) e enviar para o servidor (pacman_play)
 *  8) Cleanup: join, disconnect, fechar ficheiros, free, terminal_cleanup
 */
int main(int argc, char *argv[]) {
    /* Aceita com ou sem ficheiro de comandos. */
    if (argc != 3 && argc != 4) {
        fprintf(stderr,
            "Usage: %s <client_id> <register_pipe> [commands_file]\n",
            argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    /* Se houver ficheiro de comandos, abre para leitura. */
    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    /* Evita terminar o processo ao escrever num pipe fechado (SIGPIPE). */
    signal(SIGPIPE, SIG_IGN);

    /* Construção dos caminhos dos FIFOs privados deste cliente. */
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    /* Inicializa debug para um ficheiro local do cliente. */
    open_debug_file("client-debug.log");

    /* Remove FIFOs antigos (execuções anteriores). */
    unlink(req_pipe_path);
    unlink(notif_pipe_path);

    /* Cria FIFO de pedidos (cliente -> servidor). */
    if (mkfifo(req_pipe_path, 0666)< 0) {
        perror("Failed to create request pipe");
        return 1;
    }

    /* Cria FIFO de notificações (servidor -> cliente). */
    if (mkfifo(notif_pipe_path, 0666) < 0) {
        perror("Failed to create notification pipe");
        unlink(req_pipe_path);
        return 1;
    }

    /* Faz handshake de ligação com o servidor através do FIFO de registo. */
    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0) {
        perror("Failed to connect to server");
        unlink(req_pipe_path);
        unlink(notif_pipe_path);
        return 1;
    }

    /* Buffer fixo para o tabuleiro (grid linear), preenchido pela API. */
    tabuleiro = malloc(MAX_BOARD_CELLS);
    if(!tabuleiro){
        perror("malloc tabuleiro");
        pacman_disconnect();
        return 1;
    }

    /* Inicializa UI ncurses. */
    terminal_init();
    set_timeout(500);
    clear();
    mvprintw(0, 0, "A ligar ao servidor...");
    refresh_screen();

    /* Lança thread que recebe e desenha updates do servidor. */
    pthread_t receiver_thread_id;
    if(pthread_create(&receiver_thread_id, NULL, receiver_thread, NULL) != 0){
        perror("pthread_create");
        pacman_disconnect();
        free(tabuleiro);
        terminal_cleanup();
        return 1;
    }

    /* Loop principal: lê comandos e envia para o servidor. */
    char command;
    int ch;
    bool disconnected; /* Nota: devia iniciar a false para evitar lixo. */

    while (1) {

        /* Se a receiver_thread sinalizou paragem, termina. */
        pthread_mutex_lock(&mutex);
        bool stop = stop_execution;
        pthread_mutex_unlock(&mutex);
        if(stop)
            break;

        if (cmd_fp) {
            /* Input vindo de ficheiro de comandos. */
            ch = fgetc(cmd_fp);

            if (ch == EOF) {
                /* Se chegar ao fim, recomeça o ficheiro (loop infinito de comandos). */
                rewind(cmd_fp);
                continue;
            }

            command = (char)ch;

            /* Ignora caracteres não-comando. */
            if (command == '\n' || command == '\r' || command == '\0')
                continue;

            /* Normaliza para maiúsculas. */
            command = (char)toupper((unsigned char) command);
            
            /* Espera "tempo" ms para não encher o pipe de requests. */
            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            pthread_mutex_unlock(&mutex);

            sleep_ms(wait_for);
            
        } else {
            /* Input interativo (teclado). */
            command = get_input();
            command = (char)toupper((unsigned char)command);
        }

        if (command == '\0')
            continue;

        /* 'Q' termina o cliente e notifica o servidor. */
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

        /* Envia comando de movimento/ação ao servidor. */
        if(pacman_play(command) < 0){
            debug("pacman_play failed\n");
            break;
        }

    }

    /* Garante que a receiver_thread sai. */
    pthread_mutex_lock(&mutex);
    stop_execution = true;
    pthread_mutex_unlock(&mutex);

    /* Espera pela thread de receção. */
    pthread_join(receiver_thread_id, NULL);

    /* Se ainda não desconectou explicitamente, fecha sessão no servidor. */
    if(!disconnected) pacman_disconnect();

    /* Cleanup de recursos. */
    if (cmd_fp)
        fclose(cmd_fp);

    free(tabuleiro);
    pthread_mutex_destroy(&mutex);

    terminal_cleanup();
    fflush(stdout);

    return 0;
}
