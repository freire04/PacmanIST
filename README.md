# PacmanIST

PacmanIST is a concurrent client-server implementation of a Pac-Man-style game developed in C as a two-person project for the Operating Systems course at Instituto Superior Técnico.

Players navigate a two-dimensional maze, collect points, move between levels through portals and avoid monsters. In this version of the project, the game runs as a standalone server capable of managing multiple simultaneous game sessions.

Each client connects to the server through named pipes (FIFOs), sends player commands and receives periodic updates containing the current game state.

This repository contains the second and final part of the project, focused on client-server communication and concurrent game sessions.

## Features

- Concurrent support for multiple independent game sessions
- Client-server architecture
- Communication through named pipes (FIFOs)
- Dedicated threads for concurrent game execution
- Synchronization of shared game state
- Periodic game-state updates sent from server to clients
- Terminal-based interface using `ncurses`
- POSIX file and I/O operations

## Architecture

The project is divided into two main components:

### Server

The server manages the game logic and accepts connections from multiple clients.  
Each game session is handled independently, allowing several Pac-Man games to run concurrently.

The server:
- receives client connection requests;
- manages active game sessions;
- processes player commands;
- updates Pac-Man and monster state concurrently;
- sends updated board states back to each client.

### Client

Each client establishes a session with the server using dedicated FIFOs.

The client:
- sends Pac-Man movement commands to the server;
- receives periodic game-state updates;
- renders the board in the terminal using `ncurses`;
- handles connection and disconnection from the server.

## Technologies & Concepts

- C
- POSIX
- pthreads
- Mutexes / synchronization
- Named pipes (FIFOs)
- Inter-process communication (IPC)
- Client-server architecture
- ncurses
- Make

## Project Structure

```text
PacmanIST/
├── client-side/
│   ├── src/client/      # Client application and communication logic
│   ├── include/         # Client headers and communication protocol
│   └── Makefile
│
└── server-side/
    ├── src/             # Game server, board logic and parsing
    ├── include/         # Server headers and shared protocol definitions
    ├── levels/          # Game levels and character behaviour files
    └── Makefile


```
## Build & Run

### Requirements

- GCC with C17 support
- Make
- `ncurses`
- A POSIX-compatible environment

### Build the server

```bash
cd server-side
make
```

Start the server with:

```bash
./bin/Pacmanist <level_directory> <max_games> <register_fifo>
```

Example:

```bash
./bin/Pacmanist levels 4 /tmp/pacmanist_register
```

The server creates the registration FIFO and waits for clients to connect.

### Build the client

In another terminal:

```bash
cd client-side
make
```

Start a client with:

```bash
./bin/client <client_id> <register_fifo> [commands_file]
```

Example:

```bash
./bin/client 1 /tmp/pacmanist_register
```

If no command file is provided, player commands are read interactively from standard input.

Multiple clients can be launched with different IDs to create independent game sessions:

```bash
./bin/client 2 /tmp/pacmanist_register
./bin/client 3 /tmp/pacmanist_register
```

### Clean build files

Run inside either `client-side/` or `server-side/`:

```bash
make clean
```

## Academic Context

Developed as a two-person project for the Operating Systems course at Instituto Superior Técnico.

This repository contains the final stage of the project, extending the original PacmanIST implementation with a concurrent client-server architecture and inter-process communication through named pipes (FIFOs).
