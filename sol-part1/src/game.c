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

#include "board.h"
#include "display.h"
#include "protocol.h"

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

typedef struct{
    char fifo_registo[MAX_PIPE_PATH_LENGTH];
    int max_games;
    char *levels_path;

}host_thread_arg_t;

typedef struct {
    board_t *board;
    int req_fd;
    volatile int *running;
    volatile int *victory;
} pacman_thread_arg_t;

typedef struct {
    int client_fd;
    volatile int *running;
    volatile int *victory;
    int req_fd;
    int notif_fd;
    char req_pipe[MAX_PIPE_PATH_LENGTH];
    char notif_pipe[MAX_PIPE_PATH_LENGTH];
    
} client_thread_arg_t;

typedef struct {
    int op_code;                           // OP_CODE_CONNECT, OP_CODE_PLAY, etc
    char req_pipe[MAX_PIPE_PATH_LENGTH];   // Path do FIFO de pedidos
    char notif_pipe[MAX_PIPE_PATH_LENGTH]; // Path do FIFO de notificações
    char command;                          // Para OP_CODE_PLAY
} message_t;

static void* pacman_thread(void *arg);
static void* ghost_thread(void *arg);
static void* host_thread(void *arg);
static void* ncurses_thread(void *arg);
static void* sender_thread(void *arg);

int thread_shutdown = 0;
static int read_full(int fd, void *buff, size_t n);
static int write_full(int fd, const void *buf, size_t n);


static int write_full(int fd, const void *buf, size_t n){
    size_t off = 0;
    while(off < n){
        ssize_t w = write(fd, (const char*)buf + off, n - off);
        if(w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
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

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();     
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

            // detectar se há fantasma carregado nesta célula
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
                    out[pos++] = ' '; // fallback seguro
                    break;
            }
        }
    }

    return out;
}

static void* sender_thread(void *arg){
    sender_args_t *a = (sender_args_t*)arg;

    while (*a->running) {
        sleep_ms(a->board->tempo);

        int width, height, tempo, victory, game_over, points;
        char *grid = NULL;

        pthread_rwlock_rdlock(&a->board->state_lock);
        width  = a->board->width;
        height = a->board->height;
        tempo  = a->board->tempo;
        points = a->board->pacmans[0].points;
        game_over = (a->board->pacmans[0].alive == 0);
        victory   = (*a->victory);

        grid = build_board_data_for_client(a->board); // malloc
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

void* host_thread(void *arg) {
    host_thread_arg_t *host_arg = (host_thread_arg_t*) arg;
    
    char fifo_registo[MAX_PIPE_PATH_LENGTH];
    strcpy(fifo_registo, host_arg->fifo_registo);
    int max_games = host_arg->max_games;
    char levels_path[256];
    strcpy(levels_path, host_arg->levels_path);
    free(arg);

    int reg_fd = open(fifo_registo, O_RDWR);
    if (reg_fd < 0) {
        perror("host_thread: open fifo_registo");
        return NULL;
    }

    int games_played = 0;

    while (1) {
        char op = 0;
        char req_pipe[MAX_PIPE_PATH_LENGTH] = {0};
        char notif_pipe[MAX_PIPE_PATH_LENGTH] = {0};

        int rr = read_full(reg_fd, &op, 1);
        if (rr <= 0) continue;

        if (op != OP_CODE_CONNECT) {
            debug("host_thread: invalid op_code %d\n", (int) op);
            continue;
        }

        if (read_full(reg_fd, req_pipe, MAX_PIPE_PATH_LENGTH) <= 0) continue;
        if (read_full(reg_fd, notif_pipe, MAX_PIPE_PATH_LENGTH) <= 0) continue;

        req_pipe[MAX_PIPE_PATH_LENGTH - 1] = '\0';
        notif_pipe[MAX_PIPE_PATH_LENGTH - 1] = '\0';

        if (max_games > 0 && games_played >= max_games) {
            debug("host_thread: max games reached\n");
            continue;
        }

        games_played++;

        int notif_fd = open(notif_pipe, O_WRONLY);
        if (notif_fd < 0) {
            perror("host_thread: open notif_pipe");
            games_played--;
            continue;
        }

        int req_fd = open(req_pipe, O_RDONLY);
        if (req_fd < 0) {
            perror("host_thread: open req_pipe");
            close(notif_fd);
            games_played--;
            continue;
        }

        client_thread_arg_t *client_arg = malloc(sizeof(client_thread_arg_t));
        if (!client_arg) {
            perror("host_thread: malloc client_arg");
            close(req_fd);
            close(notif_fd);
            games_played--;
            continue;
        }

        client_arg->req_fd = req_fd;
        client_arg->notif_fd = notif_fd;
        strncpy(client_arg->level_dir_path, levels_path, sizeof(client_arg->level_dir_path) - 1);
        client_arg->level_dir_path[sizeof(client_arg->level_dir_path) - 1] = '\0';

        pthread_t client_tid;
        if (pthread_create(&client_tid, NULL, client_thread, client_arg) != 0) {
            perror("host_thread: pthread_create client_thread");
            close(req_fd);
            close(notif_fd);
            free(client_arg);
            games_played--;
            continue;
        }

        
        debug("host_thread: client_thread created\n");
    }

    close(reg_fd);
    return NULL;
}

static void* client_thread(void *arg){
    client_thread_arg_t *arg = (client_thread_arg_t*) arg;
     
}



static void* pacman_thread(void *arg) {
    pacman_thread_arg_t *p = (pacman_thread_arg_t*)arg;
    board_t *board = p->board;

    while (*p->running) {
        char op_code = 0;
        int rr = read_full(p->req_fd, &op_code, 1);
        if (rr <= 0) { // cliente morreu/fechou
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

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_wrlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <level_directory> <max_games> <fifo_registo>\n", argv[0]);
        return -1;
    }

    int max_games = atoi(argv[2]);
    char *fifo_registo = argv[3];

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    if (mkfifo(fifo_registo, 0666) == -1) {
        if (errno == EEXIST) {
            struct stat st;
            if (stat(fifo_registo, &st) == 0 && S_ISFIFO(st.st_mode)) {
                // já existe e é FIFO -> ok, reutiliza
            } else {
                // existe mas não é FIFO -> remove e cria
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

    DIR* level_dir = opendir(argv[1]);
        
    if (level_dir == NULL) {
        fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
        return 0;
    }

    open_debug_file("debug.log");

    terminal_init();
    
    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;
    signal(SIGPIPE, SIG_IGN);

    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL && !end_game) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;

        if (strcmp(dot, ".lvl") == 0) {
            load_level(&game_board, entry->d_name, argv[1], accumulated_points);
            draw_board(&game_board, DRAW_MENU);
            refresh_screen();

            while(true) {
                pthread_t ncurses_tid, host_tid;
                pthread_t *ghost_tids = NULL;

                if (game_board.n_ghosts > 0) {
                    ghost_tids = malloc((size_t)game_board.n_ghosts * sizeof(*ghost_tids));
                    if (!ghost_tids) {
                        perror("malloc ghost_tids");
                        return -1;
                    }
                }

                thread_shutdown = 0;

                debug("Creating threads\n");
                
                host_thread_arg_t *host_arg = malloc(sizeof(host_thread_arg_t));
                host_arg->board = &game_board;
                strcpy(host_arg->fifo_registo, fifo_registo);
                host_arg->max_games = max_games;
                pthread_create(&host_tid, NULL, host_thread, host_arg);

                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->board = &game_board;
                    arg->ghost_index = i;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                }
                pthread_create(&ncurses_tid, NULL, ncurses_thread, (void*) &game_board);

                void *ret = NULL;
                pthread_join(host_tid, &ret);
                int result = (int)(intptr_t) ret;

                pthread_rwlock_wrlock(&game_board.state_lock);
                thread_shutdown = 1;
                pthread_rwlock_unlock(&game_board.state_lock);

                pthread_join(ncurses_tid, NULL);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }

                free(ghost_tids);

                if(result == NEXT_LEVEL) {
                    screen_refresh(&game_board, DRAW_WIN);
                    sleep_ms(game_board.tempo);
                    break;
                }

                if(result == QUIT_GAME) {
                    screen_refresh(&game_board, DRAW_GAME_OVER); 
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    break;
                }
      
                screen_refresh(&game_board, DRAW_MENU); 

                accumulated_points = game_board.pacmans[0].points;      
            }
            print_board(&game_board);
            unload_level(&game_board);
        }
    }    

    terminal_cleanup();

    close_debug_file();

    if (closedir(level_dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
    }

    unlink(fifo_registo);
    return 0;
}