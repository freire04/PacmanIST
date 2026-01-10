#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include <semaphore.h>

#include "board.h"
#include "display.h"
#include "protocol.h"

/* ============================================================
 *  Estruturas de argumentos para threads
 * ============================================================ */

/* Argumentos da thread de cada fantasma */
typedef struct {
    board_t *board;         
    int ghost_index;         // índice do fantasma no board
    volatile int *running;   // flag partilhada: 1 enquanto a sessão está ativa 
} ghost_thread_arg_t;

/* Argumentos da thread sender (envia updates do jogo para o cliente) */
typedef struct {
    board_t *board;          
    int notif_fd;            
    volatile int *running;   // flag partilhada da sessão 
    volatile int *victory;   // flag partilhada: 1 se Pacman chegou ao portal 
} sender_args_t;

/* Argumentos da host_thread (anfitriã que aceita ligações) */
typedef struct{
    char fifo_registo[MAX_PIPE_PATH_LENGTH]; 
    char levels_dir_path[MAX_LEVEL_DIR_PATH];
    int max_games;                           // limite de sessões concorrentes (0=ilimitado) */
} host_thread_arg_t;

// Argumentos da thread do pacman (consome comandos do cliente) 
typedef struct {
    board_t *board;          
    int req_fd;             
    volatile int *running;   // flag partilhada da sessão 
    volatile int *victory;   // flag partilhada: 1 se Pacman chegou ao portal 
} pacman_thread_arg_t;

// argumentos da client_thread (uma sessão de jogo para um cliente) 
typedef struct {
    int req_fd;              
    int notif_fd;            
    char level_dir_path[256]; // diretorio dos níveis
} client_thread_arg_t;

// funcoes das threads
static void* pacman_thread(void *arg);
static void* ghost_thread(void *arg);
static void* host_thread(void *arg);
static void* sender_thread(void *arg);
static void* client_thread(void *arg);

// helpers
static int read_full(int fd, void *buff, size_t n);
static int write_full(int fd, const void *buf, size_t n);

// controlo das sessoes
static sem_t sem_slots;     // semaforo com slots das sessoes
static int sem_enabled = 0; // 1 se max_games>0


// fiz isto para ser mais legivel quando o servidor esta a correr
// e para tambem ter alguma info sobre o que esta a fazer
static void print_server_banner(const char *fifo, int max_games, const char *levels_dir) {
    printf("\n");
    printf("////////////////////////////////////////\n");
    printf("//                                    //\n");
    printf("//            SERVER STARTED           //\n");
    printf("//                                    //\n");
    printf("////////////////////////////////////////\n");
    printf("Levels:    %s\n", levels_dir);
    printf("Max games: %d%s\n", max_games, (max_games == 0 ? " (unlimited)" : ""));
    printf("FIFO:      %s\n\n", fifo);
    fflush(stdout);
}

/*
 *  write_full:
 *  - garante que escreve exatamente n bytes
 *  - trata EINTR (volta a tentar)
 *  - trata EPIPE (cliente fechou o FIFO) -> erro
 *  Retorna 0 em sucesso, -1 em erro.
 * */
static int write_full(int fd, const void *buf, size_t n){
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char*)buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) return -1;
            return -1;
        }
        if (w == 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

/*
 *  read_full:
 *  - lê exatamente n bytes
 *  - trata EINTR (volta a tentar)
 *  Retorna:
 *    1  em sucesso
 *    0  em EOF (pipe fechado)
 *   -1  em erro
*/
static int read_full(int fd, void *buf, size_t n){
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, (char*)buf + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return 0;
        off += (size_t)r;
    }
    return 1;
}

// O nome esclarece-se por si proprio
void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();
}

/*
 *  build_board_data_for_client:
 *  converte o board interno para um “grid linear” (width*height)
 *  que o cliente sabe desenhar.*/
static char *build_board_data_for_client(board_t *board){
    size_t cells = (size_t)board->width * (size_t)board->height;
    if (board->width <= 0 || board->height <= 0) return NULL;
    if (cells > 1000000) return NULL;

    char *out = malloc(cells);
    if (!out) return NULL;

    size_t pos = 0;
    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;
            char c = board->board[idx].content;
            int ghost_charged = 0;
            if (c == 'M') {
                for (int g = 0; g < board->n_ghosts; g++) {
                    ghost_t *gh = &board->ghosts[g];
                    if (gh->pos_x == x && gh->pos_y == y) {
                        ghost_charged = gh->charged ? 1 : 0;
                        break;
                    }
                }
            }

            switch (c) {
                case 'W': out[pos++] = '#'; break;
                case 'P': out[pos++] = 'C'; break;
                case 'M': out[pos++] = ghost_charged ? 'G' : 'M'; break;
                case ' ':
                    if (board->board[idx].has_portal) out[pos++] = '@';
                    else if (board->board[idx].has_dot) out[pos++] = '.';
                    else out[pos++] = ' ';
                    break;
                default:
                    out[pos++] = ' ';
                    break;
            }
        }
    }

    return out;
}

/*
 *  sender_thread:
 *  Thread dedicada a enviar updates periódicos ao cliente via notif_fd.
 */
static void* sender_thread(void *arg){
    sender_args_t *a = (sender_args_t*)arg;

    while (*a->running) {
        int total = a->board->tempo;
        const int step = 20;

        // espera total ms mas em fatias para conseguir reagir a flag do running
        for (int waited = 0; waited < total; waited += step) {
            if (!*a->running) return NULL;
            sleep_ms(step);
        }

        int width, height, tempo, victory, game_over, points;
        char *grid = NULL;
        
        pthread_rwlock_rdlock(&a->board->state_lock);
        width  = a->board->width;
        height = a->board->height;
        tempo  = a->board->tempo;
        points = a->board->pacmans[0].points;
        game_over = (a->board->pacmans[0].alive == 0);
        victory   = (*a->victory);

        // converte o board interno no formato do cliente
        grid = build_board_data_for_client(a->board);
        pthread_rwlock_unlock(&a->board->state_lock);

        if (!grid) {
            *a->running = 0;
            break;
        }

        char op = OP_CODE_BOARD;
        size_t cells = (size_t)width * (size_t)height;

        if (write_full(a->notif_fd, &op, 1) < 0 ||
            write_full(a->notif_fd, &width, sizeof(int)) < 0 ||
            write_full(a->notif_fd, &height, sizeof(int)) < 0 ||
            write_full(a->notif_fd, &tempo, sizeof(int)) < 0 ||
            write_full(a->notif_fd, &victory, sizeof(int)) < 0 ||
            write_full(a->notif_fd, &game_over, sizeof(int)) < 0 ||
            write_full(a->notif_fd, &points, sizeof(int)) < 0 ||
            write_full(a->notif_fd, grid, cells) < 0) {
            free(grid);
            *a->running = 0;
            break;
        }

        free(grid);
        if (victory || game_over) {
            *a->running = 0;
            break;
        }
    }
    return NULL;
}

/*
 *  host_thread:
 *  Thread “anfitriã” do servidor:
 *   - abre o FIFO de registo
 *   - lê pedidos de conexão (OP_CODE_CONNECT + req_pipe + notif_pipe)
 *   - respeita max_games via semáforo (se ativo)
 *   - abre notif_pipe para enviar ACK
 *   - abre req_pipe para receber comandos
 *   - cria uma client_thread para gerir a sessão do cliente
 *  corre em loop infinito (server sempre pronto para receber algum pedido).
*/
void* host_thread(void *arg) {
    host_thread_arg_t *host_arg = (host_thread_arg_t*) arg;

    char fifo_registo[MAX_PIPE_PATH_LENGTH];
    char levels_path[256];

    strncpy(fifo_registo, host_arg->fifo_registo, sizeof(fifo_registo) - 1);
    fifo_registo[sizeof(fifo_registo) - 1] = '\0';

    strncpy(levels_path, host_arg->levels_dir_path, sizeof(levels_path) - 1);
    levels_path[sizeof(levels_path) - 1] = '\0';

    free(host_arg);

    int reg_fd = open(fifo_registo, O_RDWR);
    if (reg_fd < 0) {
        perror("host_thread: open fifo_registo");
        return NULL;
    }

    while (1) {
        char op = 0;
        char req_pipe[MAX_PIPE_PATH_LENGTH] = {0};
        char notif_pipe[MAX_PIPE_PATH_LENGTH] = {0};

        int rr = read_full(reg_fd, &op, 1);
        if (rr <= 0) continue;

        if (op != OP_CODE_CONNECT) {
            debug("host_thread: invalid op_code %d\n", (int)op);
            continue;
        }

        if (read_full(reg_fd, req_pipe, MAX_PIPE_PATH_LENGTH) <= 0) continue;
        if (read_full(reg_fd, notif_pipe, MAX_PIPE_PATH_LENGTH) <= 0) continue;

        req_pipe[MAX_PIPE_PATH_LENGTH - 1] = '\0';
        notif_pipe[MAX_PIPE_PATH_LENGTH - 1] = '\0';

        // limite de sessoes concorrentes (se ativado)
        if (sem_enabled) {
            while (sem_wait(&sem_slots) == -1) {
                if (errno == EINTR) continue;
                perror("host_thread: sem_wait");
                close(reg_fd);
                return NULL;
            }
        }

        // abre fifo de notifs e envia ACK
        int notif_fd = open(notif_pipe, O_WRONLY);
        if (notif_fd < 0) {
            perror("host_thread: open notif_pipe");
            if (sem_enabled) sem_post(&sem_slots);
            continue;
        }

        char ack[2] = { OP_CODE_CONNECT, 0 };
        if (write_full(notif_fd, ack, 2) != 0) {
            perror("host_thread: write ack");
            close(notif_fd);
            if (sem_enabled) sem_post(&sem_slots);
            continue;
        }

        int req_fd = open(req_pipe, O_RDONLY);
        if (req_fd < 0) {
            perror("host_thread: open req_pipe");
            close(notif_fd);
            if (sem_enabled) sem_post(&sem_slots);
            continue;
        }

        // prepara os argumentos para a thread de sessao do cliente
        client_thread_arg_t *client_arg = malloc(sizeof(*client_arg));
        if (!client_arg) {
            perror("host_thread: malloc client_arg");
            close(req_fd);
            close(notif_fd);
            if (sem_enabled) sem_post(&sem_slots);
            continue;
        }

        client_arg->req_fd = req_fd;
        client_arg->notif_fd = notif_fd;
        strncpy(client_arg->level_dir_path, levels_path,
                sizeof(client_arg->level_dir_path) - 1);
        client_arg->level_dir_path[sizeof(client_arg->level_dir_path) - 1] = '\0';

        pthread_t client_tid;
        if (pthread_create(&client_tid, NULL, client_thread, client_arg) != 0) {
            perror("host_thread: pthread_create client_thread");
            free(client_arg);
            close(req_fd);
            close(notif_fd);
            if (sem_enabled) sem_post(&sem_slots);
            continue;
        }

        pthread_detach(client_tid);
        debug("host_thread: client_thread created\n");
    }

    close(reg_fd);
    return NULL;
}

/*
 *  client_thread:
 *   - thread que gere uma sessão (um cliente).
 *   - criar:
 *      - sender_thread (envia updates)
 *      - pacman_thread (le os comandos)
 *      - ghost_threads (movimento dos fantasmas)
 *   - esperar fim do jogo e, sinaliza a flag running=0
*/
static void* client_thread(void *arg){
    client_thread_arg_t *carg = (client_thread_arg_t*) arg;

    int req_fd = carg->req_fd;
    int notif_fd = carg->notif_fd;
    char level_dir_path[256];
    strncpy(level_dir_path, carg->level_dir_path, sizeof(level_dir_path) - 1);
    level_dir_path[sizeof(level_dir_path) - 1] = '\0';
    free(carg);

    DIR* level_dir = opendir(level_dir_path);
    if (!level_dir) {
        fprintf(stderr, "client_thread: opendir failed for %s\n", level_dir_path);
        close(req_fd);
        close(notif_fd);
        if (sem_enabled) sem_post(&sem_slots);
        return NULL;
    }

    int accumulated_points = 0;

    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lvl") != 0) continue;

        board_t game_board;
        load_level(&game_board, entry->d_name, level_dir_path, accumulated_points);

        volatile int running = 1;
        volatile int victory = 0;

        pthread_t sender_tid, pacman_tid;
        pthread_t *ghost_tids = NULL;

        if (game_board.n_ghosts > 0) {
            ghost_tids = malloc((size_t)game_board.n_ghosts * sizeof(*ghost_tids));
            if (!ghost_tids) {
                unload_level(&game_board);
                break;
            }
        }

        // argumentos das threads sender e pacman
        sender_args_t *sender_arg = malloc(sizeof(*sender_arg));
        pacman_thread_arg_t *pacman_arg = malloc(sizeof(*pacman_arg));
        if (!sender_arg || !pacman_arg) {
            free(sender_arg);
            free(pacman_arg);
            unload_level(&game_board);
            free(ghost_tids);
            break;
        }

        *sender_arg = (sender_args_t){
            .board = &game_board,
            .notif_fd = notif_fd,
            .running = &running,
            .victory = &victory
        };

        *pacman_arg = (pacman_thread_arg_t){
            .board = &game_board,
            .req_fd = req_fd,
            .running = &running,
            .victory = &victory
        };

        pthread_create(&sender_tid, NULL, sender_thread, sender_arg);
        pthread_create(&pacman_tid, NULL, pacman_thread, pacman_arg);

        for (int i = 0; i < game_board.n_ghosts; i++) {
            ghost_thread_arg_t *ghost_arg = malloc(sizeof(ghost_thread_arg_t));
            if (!ghost_arg){
                running = 0;
                break;
            }
            ghost_arg->board = &game_board;
            ghost_arg->ghost_index = i;
            ghost_arg->running = &running;
            pthread_create(&ghost_tids[i], NULL, ghost_thread, ghost_arg);
        }

        pthread_join(pacman_tid, NULL);
        running = 0;

        // junta sender e fantasmas
        pthread_join(sender_tid, NULL);
        for (int i = 0; i < game_board.n_ghosts; i++) {
            pthread_join(ghost_tids[i], NULL);
        }

        free(ghost_tids);
        free(sender_arg);
        free(pacman_arg);

        if (victory) {
            accumulated_points = game_board.pacmans[0].points;
            unload_level(&game_board);
            continue;
        } else {
            // se perdeu/desconectou, termina sessão
            unload_level(&game_board);
            break;
        }
    }

    closedir(level_dir);
    close(req_fd);
    close(notif_fd);

    // liberta o slot da sessao (isto se  o semaforo estiver ativo aka max_games>0)
    if (sem_enabled) sem_post(&sem_slots);
    return NULL;
}

/* 
 *  pacman_thread:
 *  - thread que le comandos do cliente via req_fd e aplica ao board
*/
static void* pacman_thread(void *arg) {
    pacman_thread_arg_t *p = (pacman_thread_arg_t*)arg;
    board_t *board = p->board;

    while (*p->running) {
        char op_code = 0;
        int rr = read_full(p->req_fd, &op_code, 1);
        if (rr <= 0) {
            *p->running = 0;
            break;
        }

        if (op_code == OP_CODE_DISCONNECT) {
            *p->running = 0;
            break;
        }

        if (op_code == OP_CODE_PLAY) {
            char move_command = 0;
            if (read_full(p->req_fd, &move_command, 1) <= 0) {
                *p->running = 0;
                break;
            }

            command_t cmd = {.command = move_command, .turns = 1, .turns_left = 1};

            pthread_rwlock_wrlock(&board->state_lock);
            int res = move_pacman(board, 0, &cmd);
            if (res == REACHED_PORTAL) {
                *p->victory = 1;
                *p->running = 0;
            } else if (res == DEAD_PACMAN) {
                *p->running = 0;
            }
            pthread_rwlock_unlock(&board->state_lock);
        }
    }

    return NULL;
}

/*
 *  ghost_thread:
 *  - thread de um fantasma: move-se periodicamente enquanto running=1.
*/
void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    volatile int *running = ghost_arg->running;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (*running) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_wrlock(&board->state_lock);
        if (!*running) {
            pthread_rwlock_unlock(&board->state_lock);
            break;
        }

        move_ghost(board, ghost_ind,
                   &ghost->moves[ghost->current_move % ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
    return NULL;
}

/*
 *  main (server):
 *   - valida argumentos: <level_directory> <max_games> <fifo_registo>
 *   - cria/reutiliza FIFO de registo
 *   - ignora SIGPIPE
 *   - (opcional) inicia semáforo com max_games
 *   - cria host_thread
 *   - join host_thread (na prática o servidor fica “para sempre”)
 *   - cleanup: destroy sem, close_debug_file, unlink FIFO
*/
int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <level_directory> <max_games> <fifo_registo>\n", argv[0]);
        return -1;
    }

    int max_games = atoi(argv[2]);
    char *fifo_registo = argv[3];

    // banner informativo ao iniciar
    print_server_banner(argv[1], max_games, fifo_registo);

    srand((unsigned int)time(NULL));

    if (mkfifo(fifo_registo, 0666) == -1) {
        if (errno == EEXIST) {
            struct stat st;
            if (stat(fifo_registo, &st) == 0 && S_ISFIFO(st.st_mode)) {
            } else {
                unlink(fifo_registo);
                if (mkfifo(fifo_registo, 0666) == -1) {
                    perror("Erro ao criar o FIFO");
                    return -1;
                }
            }
        } else {
            perror("Erro ao criar o FIFO");
            return -1;
        }
    }

    open_debug_file("debug.log");
    // Evita terminar por SIGPIPE se um cliente fechar o pipe. */
    signal(SIGPIPE, SIG_IGN);

    // Prepara argumentos para a host_thread
    host_thread_arg_t *host_arg = malloc(sizeof(host_thread_arg_t));
    if (!host_arg) {
        perror("malloc host_arg");
        close_debug_file();
        unlink(fifo_registo);
        return -1;
    }

    strncpy(host_arg->levels_dir_path, argv[1], sizeof(host_arg->levels_dir_path) - 1);
    host_arg->levels_dir_path[sizeof(host_arg->levels_dir_path) - 1] = '\0';
    strcpy(host_arg->fifo_registo, fifo_registo);
    host_arg->max_games = max_games;

    // inicializa semaforo para limitar sessoes (caso max_games>0)
    if (max_games > 0) {
        sem_init(&sem_slots, 0, max_games);
        sem_enabled = 1;
    }

    // lanca host_thread
    pthread_t host_tid;
    if (pthread_create(&host_tid, NULL, host_thread, host_arg) != 0) {
        perror("pthread_create host_thread");
        free(host_arg);
        close_debug_file();
        unlink(fifo_registo);
        return -1;
    }

    pthread_join(host_tid, NULL);

    // cleanup (só acontece se host_thread terminar)
    if(max_games > 0) sem_destroy(&sem_slots);

    close_debug_file();
    unlink(fifo_registo);
    return 0;
}
