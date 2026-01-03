#include "board.h"
#include "display.h"
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
#include "protocol.h"
#include <sys/stat.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct {
    board_t *board;
    int notif_fd;
    volatile int *running;
    volatile int *victory;
} sender_args_t;

typedef struct {
    const char *fifo_registo;
    board_t *board;
} host_args_t;

typedef struct {
    board_t *board;
    int req_fd;
    volatile int *running;
    volatile int *victory;
} pacman_args_t;

static int read_full(int fd, void *buff, size_t n);
static int write_full(int fd, const void *buf, size_t n);
static void handle_client(const char* req_pipe_path, const char* notif_pipe_path, board_t* game_board);

int thread_shutdown = 0;


void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();     
}

static int write_full(int fd, const void *buf, size_t n){
    size_t off = 0;
    while(off < n){
        ssize_t w = write(fd, (const char*)buf + off, n - off);
        if(w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

static void *sender_thread(void *arg){
    sender_args_t *a = arg;

    while(*a->running){
        sleep_ms(a->board->tempo);

        int width, height, tempo, victory, game_over, points;

        pthread_rwlock_rdlock(&a->board->state_lock);
        width  = a->board->width;
        height = a->board->height;
        tempo  = a->board->tempo;
        points = a->board->pacmans[0].points;
        game_over = (a->board->pacmans[0].alive == 0);
        victory   = (*a->victory);
        char *grid = get_board_displayed(a->board); // malloc'ed string
        pthread_rwlock_unlock(&a->board->state_lock);

        if(!grid) {
            *a->running = 0;
            break;
        }

        char op = OP_CODE_BOARD;
        size_t cells = (size_t) width * (size_t) height;

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

        if(game_over || victory){
            *a->running = 0;
            break;
        }
    }

    return NULL;
}


void* ncurses_thread(void *arg) {
    board_t *board = (board_t*) arg;
    sleep_ms(board->tempo / 2);
    while (true) {
        sleep_ms(board->tempo);
        pthread_rwlock_wrlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        screen_refresh(board, DRAW_MENU);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* pacman_thread(void *arg) {
    // Usamos uma struct de argumentos para passar o board e o FD do pipe
    pacman_args_t *p_args = (pacman_args_t*) arg;
    board_t *board = p_args->board;
    int req_fd = p_args->req_fd;
    volatile int *running = p_args->running;
    volatile int *victory = p_args->victory;

    while (*running) {
        char op_code = 0;
        
        if (read_full(req_fd, &op_code, 1) <= 0) {
            *running = 0; // Cliente desconectou
            break;
        }

        if (op_code == OP_CODE_DISCONNECT) {
            *running = 0;
            break;
        }

        if (op_code == OP_CODE_PLAY) {
            char cmd;
            if (read_full(req_fd, &cmd, 1) <= 0) break;

            command_t play = {.command = cmd, .turns = 1};

            debug("Comando recebido : %c\n", cmd);

            
            pthread_rwlock_wrlock(&board->state_lock);
            
            int result = move_pacman(board, 0, &play);
            
            if (result == REACHED_PORTAL) {
                *victory = 1;
                *running = 0;
            } else if (result == DEAD_PACMAN || !board->pacmans[0].alive) {
                *running = 0;
            }
            
            pthread_rwlock_unlock(&board->state_lock);
        }
    }

    
    free(p_args);
    return NULL;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

static int read_full(int fd, void *buff, size_t n){
    size_t off = 0;
    while(off < n){
        ssize_t r = read(fd, (char*) buff + off, n - off);
        if (r == 0) return 0; // EOF
        if (r < 0){
            perror("read_full: read");
            return -1;
        }
        off += (size_t)r;
    }
    return 1;
}

void* host_thread(void* arg) {
    host_args_t *a = arg;
    const char *fifo_registo = a->fifo_registo;
    board_t *game_board = a->board;
    
    int reg_fd = open(fifo_registo, O_RDWR);
    if (reg_fd < 0) {
        perror("host_thread: open registro fifo");
        return NULL;
    }
    
    while (1) {
      
        char op_code = 0;
        ssize_t r = read(reg_fd, &op_code, 1);
        
        if (r == 0) {
            // EOF - reabrir FIFO para aceitar novos clientes
            close(reg_fd);
            reg_fd = open(fifo_registo, O_RDONLY);
            continue;
        }
        
        if (r < 0) {
            perror("host_thread: read op_code");
            break;
        }
        
        if (op_code != OP_CODE_CONNECT) {
            continue;
        }
        
       
        char req_pipe[MAX_PIPE_PATH_LENGTH];
        char notif_pipe[MAX_PIPE_PATH_LENGTH];
        
        if (read_full(reg_fd, req_pipe, MAX_PIPE_PATH_LENGTH) <= 0) {
            perror("host_thread: read req_pipe");
            continue;
        }
        
        if (read_full(reg_fd, notif_pipe, MAX_PIPE_PATH_LENGTH) <= 0) {
            perror("host_thread: read notif_pipe");
            continue;
        }
        
        
        handle_client(req_pipe, notif_pipe, game_board); 
    }
    
    close(reg_fd);
    return NULL;
}

void handle_client(const char* req_pipe_path, const char* notif_pipe_path, board_t* game_board) {
    int req_fd = open(req_pipe_path, O_RDONLY);
    if(req_fd < 0) {
        perror("handle_client: open req fifo (write)");
        return;
    }

    int notif_fd = open(notif_pipe_path, O_WRONLY);
    if(notif_fd < 0) {
        perror("handle_client: open notif fifo (read)");
        close(req_fd);
        return;
    }

    char response[2] = {OP_CODE_CONNECT, 0};
    if(write_full(notif_fd, response, 2) < 0){
        perror("write connect ack");
        close(req_fd);
        close(notif_fd);
        return;
    }

    volatile int running = 1;
    volatile int victory = 0;

    pthread_t send_tid;
    sender_args_t sargs = { .board = game_board, .notif_fd = notif_fd, .running = &running, .victory = &victory };
    pthread_create(&send_tid, NULL, sender_thread, &sargs);
    pacman_args_t *p_args = malloc(sizeof(pacman_args_t));
    p_args->board = game_board;
    p_args->req_fd = req_fd;
    p_args->running = &running;
    p_args->victory = &victory;

    pthread_t pacman_tid;
    pthread_create(&pacman_tid, NULL, pacman_thread, p_args);
    pthread_join(pacman_tid, NULL);

    pthread_join(send_tid, NULL);
    close(req_fd);
    close(notif_fd);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <level_directory> <max_games> <fifo_registo>\n", argv[0]);
        return -1;
    }

    int max_games = atoi(argv[2]);
    char *fifo_registo = argv[3];
    (void)max_games;

    if (mkfifo(fifo_registo, 0666) == -1) {
        perror("Erro ao criar o FIFO");
        return -1;
    }
    printf("Servidor: FIFO de registo '%s' criado com sucesso.\n", fifo_registo);


    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    DIR* level_dir = opendir(argv[1]);
        
    if (level_dir == NULL) {
        fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
        return 0;
    }

    open_debug_file("debug.log");

    //terminal_init();
    
    board_t game_board;

    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;

        if (strcmp(dot, ".lvl") == 0) {
            load_level(&game_board, entry->d_name, argv[1], 0);
            
            pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));
            thread_shutdown = 0;

            debug("Creating threads\n");
            
            // Criar threads dos fantasmas
            for (int i = 0; i < game_board.n_ghosts; i++) {
                ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                arg->board = &game_board;
                arg->ghost_index = i;
                pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
            }
            
            // Criar thread que aceita clientes
            pthread_t host_tid;
            host_args_t host_args = {.fifo_registo = fifo_registo, .board = &game_board};
            pthread_create(&host_tid, NULL, host_thread, &host_args);
            
            // Aguardar término do servidor
            pthread_join(host_tid, NULL);

            // Parar fantasmas
            pthread_rwlock_wrlock(&game_board.state_lock);
            thread_shutdown = 1;
            pthread_rwlock_unlock(&game_board.state_lock);

            for (int i = 0; i < game_board.n_ghosts; i++) {
                pthread_join(ghost_tids[i], NULL);
            }

            free(ghost_tids);
            break;
        }
    }    

    //terminal_cleanup();

    close_debug_file();

    if (closedir(level_dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
    }
    return 0;
}