#ifndef API_H
#define API_H

typedef struct {
  int width;
  int height;
  int tempo;
  int victory;
  int game_over;
  int accumulated_points;
} Board;

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path);

int pacman_play(char command);

int pacman_disconnect(void);

int receive_board_updates(char *tabuleiro);

// (helper) obter metadados do último update recebido
Board get_last_board_meta(void);

#endif