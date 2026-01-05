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

typedef struct{
    board_t *board;
    char fifo_registo[MAX_PIPE_PATH_LENGTH];
    int max_games;

}host_thread_arg_t;

typedef struct {
    board_t *board;
    int req_fd;
    int notif_fd;
} pacman_thread_arg_t;

typedef struct {
    int op_code;                           // OP_CODE_CONNECT, OP_CODE_PLAY, etc
    char req_pipe[MAX_PIPE_PATH_LENGTH];   // Path do FIFO de pedidos
    char notif_pipe[MAX_PIPE_PATH_LENGTH]; // Path do FIFO de notificações
    char command;                          // Para OP_CODE_PLAY
} message_t;

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

void* host_thread(void *arg){
    host_thread_arg_t *host_arg = (host_thread_arg_t*) arg;
    board_t *board = host_arg->board;
    char *fifo_registo = host_arg->fifo_registo;
    int max_games = host_arg->max_games;


    int games_played = 0;
    int running = 1;

    int reg_fd = open(fifo_registo, O_RDWR);
    if (reg_fd < 0) {
        perror("host_thread: open fifo_registo");
        close(reg_fd);
        return -1;
    }

    while(running){
        // Wait for a client to connect
        
        message_t msg;
        if (read_full(reg_fd, &msg, sizeof(message_t)) <= 0) {
            continue;
        }

        if(msg.op_code != OP_CODE_CONNECT) {
            debug("host_thread: invalid op_code %d\n", msg.op_code);
            continue;
        }

        if(msg.op_code == OP_CODE_CONNECT){
            debug("host_thread: trying to connect a new client\n");
            if(games_played >= max_games){
                debug("host_thread: max games reached, rejecting client\n");
                continue;
            }
            games_played++;
            
            int req_fd = open(msg.req_pipe, O_RDONLY);
            if(req_fd < 0){
                perror("host_thread: open req_pipe");
                continue;
            }
            
            
            int notif_fd = open(msg.notif_pipe, O_WRONLY);
            if(notif_fd < 0){
                perror("host_thread: open notif_pipe");
                continue;
            }

            char ack[2] = {OP_CODE_CONNECT,0};
            if (write_full(notif_fd, ack, 2) != 0) {
                perror("Erro ao enviar ACK ao cliente");
                close(req_fd);
                close(notif_fd);
                continue;
            }
            debug("host_thread: client connected successfully\n");

            pacman_thread_arg_t *p_args = malloc(sizeof(pacman_thread_arg_t));
            if (p_args == NULL) {
                close(req_fd); 
                close(notif_fd);
                continue;
            }

            p_args->board = board;
            p_args->req_fd = req_fd;
            p_args->notif_fd = notif_fd;

           
            pthread_t pacman_tid;
            if (pthread_create(&pacman_tid, NULL, pacman_thread, p_args) != 0) {
                perror("Falha ao criar thread");
                free(p_args);
                close(req_fd);
                close(notif_fd);
                continue;
            }
            debug("host_thread: pacman thread created\n");
            debug("host_thread: waiting for pacman thread to finish\n");
            pthread_join(pacman_tid, NULL);
            games_played--;
            debug("host_thread: pacman thread finished\n");
            



        }
    
    }
    close(reg_fd);
    return NULL;
}


void* pacman_thread(void *arg) {
    pacman_thread_arg_t *p_args = (pacman_thread_arg_t*) arg;
    board_t *board = p_args->board;
    int req_fd = p_args->req_fd;
    int notif_fd = p_args->notif_fd;

    pacman_t* pacman = &board->pacmans[0];


    
    while (true) {
        if(!pacman->alive) {
            break;
        }

        //sleep_ms(board->tempo * (1 + pacman->passo));

        char op_code;
        if (read_full(req_fd, &op_code, 1) <= 0) break;

        if(op_code == OP_CODE_PLAY){
            char move_command;
            if (read_full(req_fd, &move_command, 1) <= 0) break;

            command_t command;
            command.command = move_command;
            command.turns = 1;
            command.turns_left = 1;


            pthread_rwlock_wrlock(&board->state_lock);
            int move_result = move_pacman(board, 0, &command);
            

            if(move_result == REACHED_PORTAL){
                //nao sei ainda o que fazer aqui
                pthread_rwlock_unlock(&board->state_lock);
            }
            else if(move_result == DEAD_PACMAN){
                debug("pacman_thread: pacman died\n");
                pthread_rwlock_unlock(&board->state_lock);
                break;
            }
            pthread_rwlock_unlock(&board->state_lock);
        }
    }

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
        perror("Erro ao criar o FIFO");
        return -1;
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
                pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

                thread_shutdown = 0;

                debug("Creating threads\n");
                
                host_thread_arg_t host_arg;
                host_arg.board = &game_board;
                strcpy(host_arg.fifo_registo, fifo_registo);
                host_arg.max_games = max_games;
                pthread_create(&host_tid, NULL, host_thread, (void*) &host_arg);

                

                
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->board = &game_board;
                    arg->ghost_index = i;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                }
                pthread_create(&ncurses_tid, NULL, ncurses_thread, (void*) &game_board);

                int *retval;
                pthread_join(host_tid, (void**)&retval);

                pthread_rwlock_wrlock(&game_board.state_lock);
                thread_shutdown = 1;
                pthread_rwlock_unlock(&game_board.state_lock);

                pthread_join(ncurses_tid, NULL);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }

                free(ghost_tids);

                int result = *retval;
                free(retval);

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
    return 0;
}