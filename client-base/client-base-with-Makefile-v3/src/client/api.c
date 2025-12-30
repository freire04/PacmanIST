#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>

struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1, .req_pipe = -1, .notif_pipe = -1};

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

static int write_full(int fd, const void *buff, size_t n){
	size_t off = 0;
	while(off < n){
		ssize_t w = write(fd, (const char*) buff + off, n - off);
		if(w <= 0){
			perror("write_full: write");
			return -1;
		}
		off += (size_t) w;
	}

	return 0;
}

static void session_reset(void){
	if(session.req_pipe >= 0)
		close(session.req_pipe);
	if(session.notif_pipe >= 0)
		close(session.notif_pipe);

    session.id = -1;
    session.req_pipe = -1;
    session.notif_pipe = -1;
    session.req_pipe_path[0] = '\0';
    session.notif_pipe_path[0] = '\0';
}

int pacman_connect(char const *req_pipe_path, 
				   char const *notif_pipe_path, 
				   char const *server_pipe_path) {

	session_reset();

	// guardar paths na sessão
	session.id = 0; 
	strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
	session.req_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';

	strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
	session.notif_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';

	// abrir FIFO registo para escrever
	int reg_fd = open(server_pipe_path, O_WRONLY);
	if (reg_fd < 0){
		perror("pacman_connect: open server reg fifo");
		session_reset();
		return -1;
	}

	char op = OP_CODE_CONNECT;

	char repBUFFER[MAX_PIPE_PATH_LENGTH];
	char notifBUFFER[MAX_PIPE_PATH_LENGTH];
	memset(repBUFFER, 0, sizeof(repBUFFER));
	memset(notifBUFFER, 0, sizeof(notifBUFFER));

	strncpy(repBUFFER, session.req_pipe_path, MAX_PIPE_PATH_LENGTH - 1);
	strncpy(notifBUFFER, session.notif_pipe_path, MAX_PIPE_PATH_LENGTH - 1);

	if(write_full(reg_fd, &op, 1) < 0 ||
	   write_full(reg_fd, repBUFFER, MAX_PIPE_PATH_LENGTH) < 0 ||
	   write_full(reg_fd, notifBUFFER, MAX_PIPE_PATH_LENGTH) < 0){
		perror("pacman_connect: write connect");
		close(reg_fd);
		session_reset();
		return -1;
	   }
	close(reg_fd);
	
	// abrir notif FIFO para ler resposta do sv (servidor)
	session.notif_pipe = open(session.notif_pipe_path, O_RDONLY);
	if(session.notif_pipe < 0){
		perror("pacman_connect: open notif fifo (read)");
		session_reset();
		return -1;
	}
	
	// ler resposta
	char resp_op = 0;
	char result = 1;
	int rr = 0;
	
	rr = read_full(session.notif_pipe, &resp_op, 1);
	if(rr <= 0){
		perror("pacman_connect: read response op");
		session_reset();
		return -1;
	}

	rr = read_full(session.notif_pipe, &result, 1);
	if(rr <= 0){
		perror("pacman_connect: read responde result");
		session_reset();
		return -1;
	}

	// ligacao recusada ou resposta inv
	if(resp_op != OP_CODE_CONNECT || result != 0){
		session_reset();
		return -1;
	}

	// abrir req FIFO para escrever
	session.req_pipe = open(session.req_pipe_path, O_WRONLY);
	if(session.req_pipe < 0){
		perror("pacman_connect: open req fifo (write)");
		session_reset();
		return -1;
	}
	
	return 0;
}

void pacman_play(char command) {
	if(session.req_pipe < 0) return;

	char op = OP_CODE_PLAY;
	
	if(write_full(session.req_pipe, &op, 1) < 0){
		perror("pacman_play: write op");
		return;
	}
	
	if(write_full(session.req_pipe, &command, 1) < 0){
		perror("pacman_play: write command");
		return;
	}
}

int pacman_disconnect() {
  	if(session.req_pipe >= 0){
		char op = OP_CODE_DISCONNECT;

		if(write_full(session.req_pipe, &op, 1) < 0){
			perror("pacman_disconnect: write disconnect");
		}
	}
	session_reset();
	return 0;
}

Board receive_board_update(void) {
	Board board = {0};
	
	if(session.notif_pipe < 0){
		debug("receive_board_update: notif pipe not open\n");
		return board;
	}
	
	// Ler OP_CODE
	char op_code = 0;
	if(read_full(session.notif_pipe, &op_code, 1) <= 0){
		debug("receive_board_update: failed to read op_code\n");
		return board;
	}
	
	if(op_code != OP_CODE_BOARD){
		debug("receive_board_update: incorrect op_code %d\n", op_code);
		return board;
	}
	
	// Ler width
	if(read_full(session.notif_pipe, &board.width, sizeof(int)) <= 0){
		debug("receive_board_update: failed to read width\n");
		return board;
	}
	
	// Ler height
	if(read_full(session.notif_pipe, &board.height, sizeof(int)) <= 0){
		debug("receive_board_update: failed to read height\n");
		return board;
	}

	// Ler tempo
	if(read_full(session.notif_pipe, &board.tempo, sizeof(int)) <= 0){
		debug("receive_board_update: failed to read tempo\n");
		return board;
	}
	
	// Ler victory
	if(read_full(session.notif_pipe, &board.victory, sizeof(int)) <= 0){
		debug("receive_board_update: failed to read victory\n");
		return board;
	}
	
	// Ler game_over
	if(read_full(session.notif_pipe, &board.game_over, sizeof(int)) <= 0){
		debug("receive_board_update: failed to read game_over\n");
		return board;
	}
	
	// Ler accumulated_points
	if(read_full(session.notif_pipe, &board.accumulated_points, sizeof(int)) <= 0){
		debug("receive_board_update: failed to read accumulated_points\n");
		return board;
	}
	
	// Alocar memória para os dados do tabuleiro
	int board_size = board.width * board.height;
	board.data = malloc(board_size);
	if(!board.data){
		perror("receive_board_update: malloc failed");
		return board;
	}
	
	// Ler board_data
	if(read_full(session.notif_pipe, board.data, board_size) <= 0){
		debug("receive_board_update: failed to read board_data\n");
		free(board.data);
		board.data = NULL;
		return board;
	}
	
	return board;
}