# PacmanIST

PacmanIST is a concurrent client-server implementation of a Pac-Man-style game developed in C for the Operating Systems course at Instituto Superior Técnico.

Players navigate a two-dimensional maze, collect points, move between levels through portals and avoid monsters. In this version of the project, the game runs as a standalone server capable of managing multiple simultaneous game sessions.

Each client connects to the server through named pipes (FIFOs), sends player commands and receives periodic updates containing the current game state.

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
