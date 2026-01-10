#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

// Estrutura para manter o estado da sessao atual do cliente
struct Session {
  int id; // ID do cliente 
  int req_pipe; // Descritor de ficheiro (FD) para o pipe de PEDIDOS (Cliente -> Servidor)
  int notif_pipe; // Descritor de ficheiro (FD) para o pipe de NOTIFICACOES (Servidor -> Cliente)
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1]; // Caminho (string) do pipe de pedidos
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1]; // Caminho (string) do pipe de notificacoes
};

// Variavel global para guardar os dados do ultimo tabuleiro recebido
// Isto permite que outras partes do cliente acedam a pontuacao, dimensoes, etc.
static Board last_meta = {0}; 

// Funcao que devolve os dados do ultimo tabuleiro
Board get_last_board_meta(void){
    return last_meta;
}

// Instancia global da sessao, inicializada com valores de "desconectado" (-1)
static struct Session session = {.id = -1, .req_pipe = -1, .notif_pipe = -1}; 

// Funcao auxiliar para ler EXATAMENTE 'n' bytes de um descritor de ficheiro
// Necessaria porque em pipes/sockets, o read() pode retornar menos dados do que o pedido
static int read_full(int fd, void *buf, size_t n){ 
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, (char*)buf + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue; // Tentar novamente se interrompido por sinal
            return -1; // Erro real
        }
        if (r == 0) return 0; // EOF (O servidor fechou a conexão)
        off += (size_t)r;
    }
    return 1; // Sucesso (Leu tudo)
}

// Funcao auxiliar para escrever EXATAMENTE 'n' bytes num descritor de ficheiro
static int write_full(int fd, const void *buf, size_t n){ 
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char*)buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue; // Tentar novamente se interrompido
            if (errno == EPIPE) return -1; // Pipe quebrado (O servidor fechou/morreu)
            return -1; // Outro erro
        }
        if (w == 0) return -1; // Nao deve acontecer em escrita a menos que n=0
        off += (size_t)w;
    }
    return 0; // Sucesso
}

// Reinicia a sessao: fecha os pipes abertos e limpa os dados da estrutura
static void session_reset(void){ 
    if(session.req_pipe >= 0)
        close(session.req_pipe); // Fecha o pipe de pedidos
    if(session.notif_pipe >= 0)
        close(session.notif_pipe); // Fecha o pipe de notificacoes

    session.id = -1; 
    session.req_pipe = -1; 
    session.notif_pipe = -1; 
    session.req_pipe_path[0] = '\0'; // Limpa as strings dos caminhos
    session.notif_pipe_path[0] = '\0'; 
    memset(&last_meta, 0,   sizeof(last_meta)); // Limpa o estado do ultimo jogo
}

// Conecta ao servidor do Pacman
// 1. Guarda os caminhos dos pipes.
// 2. Envia pedido CONNECT através do FIFO de registo do servidor.
// 3. Abre pipe de notificacoes para receber o ACK.
// 4. Abre pipe de pedidos para enviar comandos.
int pacman_connect(char const *req_pipe_path, 
                   char const *notif_pipe_path, 
                   char const *server_pipe_path) {

    session_reset(); // Garantir estado limpo antes de conectar

    // Guardar os caminhos fornecidos pelo main.c
    session.id = 0; 
    strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
    session.req_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';

    strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
    session.notif_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';

    // Abrir o fifo publico do servidor (Registo) para escrita
    int reg_fd = open(server_pipe_path, O_WRONLY);
    if (reg_fd < 0){
        perror("pacman_connect: erro ao abrir fifo do servidor");
        session_reset();
        return 1;
    }

    char op = OP_CODE_CONNECT;

    // Preparar buffers de tamanho fixo com os caminhos dos pipes para enviar ao servidor
    char repBUFFER[MAX_PIPE_PATH_LENGTH];
    char notifBUFFER[MAX_PIPE_PATH_LENGTH];
    memset(repBUFFER, 0, sizeof(repBUFFER));
    memset(notifBUFFER, 0, sizeof(notifBUFFER));

    strncpy(repBUFFER, session.req_pipe_path, MAX_PIPE_PATH_LENGTH - 1);
    strncpy(notifBUFFER, session.notif_pipe_path, MAX_PIPE_PATH_LENGTH - 1);

    // Enviar Pedido de Conexao: [OP_CODE] + [REQ_PIPE_PATH] + [NOTIF_PIPE_PATH]
    if(write_full(reg_fd, &op, 1) < 0 ||
       write_full(reg_fd, repBUFFER, MAX_PIPE_PATH_LENGTH) < 0 ||
       write_full(reg_fd, notifBUFFER, MAX_PIPE_PATH_LENGTH) < 0){
        perror("pacman_connect: erro ao escrever connect");
        close(reg_fd);
        session_reset();
        return 1;
       }
    close(reg_fd); // Ja nao precisamos do pipe de registo
    
    // Abrir FIFO de Notificacoes para LEITURA (Servidor -> Cliente)
    session.notif_pipe = open(session.notif_pipe_path, O_RDONLY);
    if(session.notif_pipe < 0){
        perror("pacman_connect: erro ao abrir notif fifo (leitura)");
        session_reset();
        return 1;
    }
    
    // Ler confirmacao (ACK) do servidor
    char resp_op = 0;
    char result = 1;
    int rr = 0;
    
    // Espera OP_CODE_CONNECT + Codigo de Resultado (0 para sucesso)
    rr = read_full(session.notif_pipe, &resp_op, 1);
    if(rr <= 0){
        perror("pacman_connect: erro ao ler op de resposta");
        session_reset();
        return 1;
    }

    rr = read_full(session.notif_pipe, &result, 1);
    if(rr <= 0){
        perror("pacman_connect: erro ao ler resultado da resposta");
        session_reset();
        return 1;
    }

    // Verificar se o servidor aceitou
    if(resp_op != OP_CODE_CONNECT || result != 0){
        session_reset();
        return 1; // recusado
    }

    // Abrir FIFO de Pedidos para ESCRITA (Cliente -> Servidor)
    session.req_pipe = open(session.req_pipe_path, O_WRONLY);
    if(session.req_pipe < 0){
        perror("pacman_connect: erro ao abrir req fifo (escrita)");
        session_reset();
        return 1;
    }
    
    return 0;
}

// Envia comandos para o servidor
int pacman_play(char command) {
    if(session.req_pipe < 0) 
        return -1; // Nao esta conectado

    char op = OP_CODE_PLAY;
    // Envia: [OP_CODE_PLAY]
    if(write_full(session.req_pipe, &op, 1) < 0){
        perror("pacman_play: erro ao escrever op");
        return -1;
    }
    
    // Envia: [TECLA] ('W', 'A', 'S', 'D')
    if(write_full(session.req_pipe, &command, 1) < 0){
        perror("pacman_play: erro ao escrever comando");
        return -1;
    }
    return 0;
}

// Desconecta do servidor e limpa os recursos
int pacman_disconnect(void) {
    int err = 0;

    // Envia opcode DISCONNECT para o servidor parar o loop do jogo
    if (session.req_pipe >= 0) {
        char op = OP_CODE_DISCONNECT;
        if (write_full(session.req_pipe, &op, 1) < 0) {
            perror("pacman_disconnect: erro ao escrever disconnect");
            err = 1;
        }
    }

    // apaga os pipes nomeados do sistema de ficheiros
    if (session.req_pipe_path[0] != '\0') {
        if (unlink(session.req_pipe_path) < 0) {
            perror("pacman_disconnect: erro unlink req pipe");
            err = 1;
        }
    }

    if (session.notif_pipe_path[0] != '\0') {
        if (unlink(session.notif_pipe_path) < 0) {
            perror("pacman_disconnect: erro unlink notif pipe");
            err = 1;
        }
    }

    session_reset(); // Fecha FDs e reinicia struct
    return err; // 0 sucesso, 1 erro
}

// Le uma atualizacao do tabuleiro do pipe de notificacoes
int receive_board_updates(char *tabuleiro) {
    if (session.notif_pipe < 0) {
        debug("receive_board_updates: pipe notif fechado\n");
        return -1;
    }

    // Ler Opcode 
    char op_code = 0;
    if (read_full(session.notif_pipe, &op_code, 1) <= 0) {
        debug("receive_board_updates: falha ao ler op_code\n");
        return -1;
    }

    if (op_code != OP_CODE_BOARD) {
        debug("receive_board_updates: op_code incorreto %d\n", (int)op_code);
        return -1;
    }

    // Ler dados (Dimensoes, Pontuacao, Estado)
    if (read_full(session.notif_pipe, &last_meta.width, sizeof(int)) <= 0) return -1;
    if (read_full(session.notif_pipe, &last_meta.height, sizeof(int)) <= 0) return -1;
    if (read_full(session.notif_pipe, &last_meta.tempo, sizeof(int)) <= 0) return -1;
    if (read_full(session.notif_pipe, &last_meta.victory, sizeof(int)) <= 0) return -1;
    if (read_full(session.notif_pipe, &last_meta.game_over, sizeof(int)) <= 0) return -1;
    if (read_full(session.notif_pipe, &last_meta.accumulated_points, sizeof(int)) <= 0) return -1;

    // Validar dimensoes recebidas
    if (last_meta.width <= 0 || last_meta.height <= 0) {
        debug("receive_board_updates: dimensoes invalidas %d x %d\n",
              last_meta.width, last_meta.height);
        return -1;
    }

    size_t board_size = (size_t)last_meta.width * (size_t)last_meta.height;

    //  Ler a Grelha do Tabuleiro
    if (read_full(session.notif_pipe, tabuleiro, board_size) <= 0) {
        debug("receive_board_updates: falha ao ler dados do tabuleiro\n");
        return -1;
    }

    return 0; 
}