# Shadow-Grid: High-Performance Concurrent Game Engine

Shadow-Grid is a custom-built, heavily optimized multiplayer game engine written entirely in C. It is designed from the ground up to handle high-concurrency environments, complex state management, and resilient data recovery natively.

Unlike standard monolithic game servers, Shadow-Grid features a proprietary architecture utilizing custom Producer-Consumer networking queues, a dual-strategy deadlock control system, a POSIX semaphore connection limiter, a buffered IPC logging pipeline, and a Write-Ahead Log (WAL) crash recovery system.

---

## 🚀 Core Technical Features

### 1. POSIX Semaphore — Hard Connection Limit (100 Players)
The server enforces a strict **maximum of 100 concurrent players** using a POSIX counting semaphore (`sem_t client_sem`), initialized to `100`.

*   **Mechanism:** The main `accept()` loop is **gated** behind a `sem_wait(&client_sem)` call. No new connection is accepted until a semaphore slot is available. When a client disconnects or quits, `sem_post(&client_sem)` releases the slot, allowing the next queued connection in.
*   **Tradeoff:** This is a deliberate architectural tradeoff — hardcoding the ceiling prevents resource exhaustion and thread starvation under flood attacks, at the cost of rejecting legitimate connections above the limit. Connections beyond 100 block at the OS TCP backlog (`listen(server_fd, 10)`) and timeout.
*   **Validation:** The `tests/test_concurrency.py` script confirms this: 105 threads race to connect, exactly 100 are accepted and exactly 5 are blocked.

### 2. Dual-Strategy Deadlock Control (Avoidance + Detection)
The engine uses two different strategies for two different locking scenarios:

*   **Strategy A — Deadlock Avoidance via Strict Global Lock Ordering (`create_squad`):**
    When batch-creating a squad, the server collects all target Player IDs, sorts them in strictly **ascending numerical order**, and then acquires their fine-grained `player_locks` in that fixed order. Since all threads follow the same ordering, cyclic wait is mathematically impossible (no two threads can each hold what the other needs). Locks are released in reverse order after the squad assignment.

*   **Strategy B — Deadlock Detection & Recovery via Wait-For Graph DFS (`invite`):**
    For dynamic, peer-to-peer invite requests, strict ordering is not feasible. Instead, the server maintains a 2D boolean adjacency matrix `WFG[MAX_PLAYERS][MAX_PLAYERS]`. When Player A waits for Player B's lock, `WFG[A][B] = 1` is set. A dedicated background **Reaper Thread** wakes every 2 seconds and performs a **Depth-First Search (DFS)** cycle detection over this matrix. If a cycle is found (e.g., A→B and B→A are simultaneously waiting), the Reaper sets `deadlock_abort[victim] = 1`, safely breaking the deadlock and returning `RESP:ERR;MSG:INVITE_FAILED_DEADLOCK_BROKEN` to the victim thread.

### 3. Lock-Free Network Broadcasting (Producer-Consumer Queue)
To prevent a slow or lagging client's TCP `send()` from stalling the entire game loop, all broadcast messages are decoupled through a custom bounded buffer.

*   **Architecture:** A circular `bcast_queue[1024]` of heap-allocated string pointers is managed by two semaphores (`bcast_sem_empty` initialized to `1024`, `bcast_sem_full` initialized to `0`) and a single `bcast_mutex`.
*   **Producer:** The main game thread (`handle_client`) calls `broadcast()`, which `strdup`s the message, acquires the empty semaphore, pushes the pointer into the queue head, and signals the full semaphore. This is **non-blocking** and returns instantly.
*   **Consumer:** A permanently detached `broadcast_thread_func` thread waits on the full semaphore, dequeues the message, iterates over all active `client_sockets[]`, and performs the actual `send()` calls. It then `free()`s the message and signals the empty semaphore.
*   **Tradeoff:** Queue capacity is capped at 1024 messages. Under extreme load (100 clients all firing simultaneously), a producer will block if the consumer falls behind. The queue size of 1024 was chosen as a balance between memory usage and flood tolerance.

### 4. Forked IPC Logger with Buffered Flush Algorithm (`game.log`)
Event logging is handled entirely by a **dedicated child process** to avoid blocking the server's game loop on slow disk I/O.

*   **IPC Mechanism:** The parent server process (`main.c`) `fork()`s a logger child at startup. Communication is via a **POSIX Shared Memory segment** (`shm_open`, `mmap`) of size 4096 bytes named `/game_log_shm_<PORT>`.
*   **Write Path:** The server's `log_event()` writes an event string to `shm_ptr`. The logger child polls `shm_ptr` every 1ms, copies it into a local `log_buffer[16384]`, and clears the shared segment.
*   **Buffered Flush Strategy (Tradeoff Analysis):**
    The logger does **not** write to disk on every event. Instead it flushes the in-memory `log_buffer` to `game.log` only when **one of three conditions** is met:
    1.  **Time Threshold:** ≥ 2 seconds have elapsed since the last flush (`FLUSH_INTERVAL = 2.0`).
    2.  **Buffer Capacity:** The accumulated buffer is at risk of overflow (`current_len + data_len + 64 > sizeof(log_buffer)`).
    3.  **Parent Exit:** The parent process has terminated (detected via `getppid() == 1`), forcing an immediate final flush.
    
    This strategy deliberately trades **durability** (a crash within the 2-second window can lose log entries) for **performance** (vastly reduces disk I/O syscalls under high event rates). File-level locking (`fcntl F_WRLCK`) prevents concurrent write corruption.

### 5. WAL Checkpointing (60-Second Interval)
A background `checkpoint_thread` runs every **60 seconds** and performs two atomic actions:
1.  Serializes the entire `players[MAX_PLAYERS]` state array and the current `event_seq` (LSN) to `game_state.dat` as a binary blob.
2.  Appends a `SEQ:<N>;EVENT:CHECKPOINT` marker to `wal.log`, so the recovery tool knows where the last clean state boundary is.

### 6. WAL-CLR Crash Recovery (Korth DBMS Model)
The external `wal_clr` tool implements the full **three-phase ARIES-style recovery algorithm** based on Korth DBMS theory:

*   **Phase 1 — Analysis:** Loads `game_state.dat` to find the last checkpoint LSN. Scans `wal.log` forward from that LSN to classify every transaction:
    *   **T1 (Winner):** Started before checkpoint, committed (QUIT) before crash → **Redo**.
    *   **T2 (Winner):** Started after checkpoint, committed before crash → **Redo**.
    *   **T3 (Loser):** Started before checkpoint, still active at crash → **Undo + Abort**.
    *   **T4 (Loser):** Started after checkpoint, still active at crash → **Undo + Abort**.
*   **Phase 2 — Redo:** Replays all log entries from `cp_lsn + 1` to `crash_lsn` to restore the intermediate state, including work done by losers (as per the "repeat history" rule).
*   **Phase 3 — Undo:** Scans the log in **reverse** from `crash_lsn` back to `cp_lsn + 1`. For each operation belonging to a loser transaction, a **Compensation Log Record (CLR)** is appended to `wal.log` (e.g., `EVENT:MOVE` is undone by writing the pre-image position back; `EVENT:JOIN` is undone by writing `EVENT:QUIT`). A final `EVENT:ABORT` record is logged for each loser, and a new clean checkpoint is saved.

---

## 📁 Project Structure

```text
├── Makefile             # One-click build script
├── src/                 # Core Engine Source Code
│   ├── client/          # Terminal-based TCP Client
│   ├── network.c        # TCP multiplexing & Producer-Consumer queues
│   ├── state.c          # Game state & Fine-Grained Locking arrays
│   ├── event.c          # Protocol parsing
│   ├── ipc.c            # Shared memory logger bridge
│   ├── auth.c           # Role-based permission checks
│   └── main.c           # Thread initializers (Reaper, Checkpoint, Logger fork)
├── include/             # Header files & Data structures
│   ├── common.h         # Global constants (PORT, SHOT_DAMAGE, ADMIN_PASS)
│   ├── state.h          # Player struct, WFG matrix, lock declarations
│   ├── protocol.h       # EventType enum, Role constants, CMD_ defines
│   ├── auth.h           # can_move / can_shoot declarations
│   ├── ipc.h            # SHM constants and log_event declaration
│   └── network.h        # Network function declarations
├── tools/               # External Recovery Utilities
│   ├── replay.c         # Rebuilds server RAM from crash logs
│   └── wal_clr.c        # Executes Undo/Redo CLR recovery (Korth model)
└── tests/               # Python test scripts
    ├── test_engine.py   # Core logic & RBAC validation (13 scenarios)
    └── test_concurrency.py  # 105-client semaphore & race condition stress test
```

---

## 🛠️ How to Build & Run

### 1. Compilation
The project uses a standard Makefile. To ensure a fresh environment without stale database logs from previous runs:
```bash
make clean && make
```
This will wipe old state files, then cleanly compile the `server`, the `client`, and the external `tools` (`replay`, `wal_clr`).

### 2. Starting the Server
```bash
./server
```
The server will initialize the semaphore, deadlock reaper thread, broadcast queue consumer thread, checkpoint thread, and the IPC logger child process, then begin listening for TCP connections on `127.0.0.1:8080`.

### 3. Starting the Client
Open a new terminal and run:
```bash
./client
```
You will automatically be assigned an ID and spawned into the grid at `(0,0)`.

---

## 🔐 Roles & Authentication (RBAC)

The engine implements a **Role-Based Access Control (RBAC)** system with three distinct roles. Every player starts as `ROLE_PLAYER` on join.

| Role | ID | Permissions |
|---|---|---|
| **Player** | `0` | `move`, `shoot`, `status`, `respawn`, `quit`, `invite` |
| **Spectator** | `1` | `status`, `quit` only — **cannot move or shoot** |
| **Admin** | `2` | All Player permissions + `kick`, `smite`, `heal_all`, `create_squad`. Also **immune to bullets** (shots targeting an Admin are automatically blocked). |

Role switching is done with the `role` command (see Game Commands below). Downgrading is free; upgrading to Admin requires a password.

---

## 🎮 Game Commands

Type these commands directly into the `client` terminal:

### Standard Player Commands
*   `move <up|down|left|right>`: Moves your character 1 space on the 20×20 grid.
*   `shoot <up|down|left|right>`: Fires a ray in a direction. Deals **50 HP** damage to the first active player hit. Blocked if target is an Admin. Blocked if target is a squadmate (Friendly Fire protection).
*   `status`: Displays your current ID, coordinates, HP, role, squad ID, and score.
*   `respawn`: Restores your HP to 100 and resets position to `(0,0)`. Only available when HP is 0.
*   `quit`: Safely disconnects, removes your entity from the grid, and releases the semaphore slot.

### Role Commands
*   `role player`: Downgrades to standard player (default).
*   `role spectator`: Enters read-only Spectator mode. Movement and shooting are disabled.
*   `role admin secret123`: Authenticates and elevates to Admin. Grants immunity and unlocks all admin commands.

### Alliance & Concurrency Commands
*   `invite <id>`: Dynamically invites a target player to your squad using a `pthread_mutex_trylock` loop. *(Triggers the Wait-For Graph Deadlock Detector if cross-invites occur simultaneously).*
*   `create_squad <id1>,<id2>`: Batch-creates a squad with multiple players atomically. *(Utilizes strict lock ordering to guarantee absolute deadlock avoidance).*

> **Friendly Fire:** Members sharing the same `squad_id` are immune to each other's bullets. Any shot that would hit a squadmate is silently blocked with `RESP:OK;MSG:FRIENDLY_FIRE_BLOCKED`.

### Admin-Only Commands
*   `kick <id>`: Forcibly drops a client's TCP socket and removes them from the grid.
*   `smite <id>`: Instantly deals **100 HP** (lethal) damage to any active target from anywhere on the map.
*   `heal_all`: Global broadcast that restores all active players to 100 HP simultaneously.

---

## 💽 Running the Recovery Tools

If the server crashes, use the built-in recovery tools to analyze and restore state.

**1. The Replay Tool**
Parses the raw `game.log` and visually rebuilds the exact state of the server right before the crash.
```bash
./replay
```

**2. The WAL-CLR Tool**
Executes advanced Korth-model transaction recovery. Runs the full Analysis → Redo → Undo pipeline and saves a clean checkpoint on completion.
```bash
./wal_clr recover
```

Additional manual modes:
```bash
./wal_clr checkpoint       # Manually force a checkpoint from current wal.log
./wal_clr redo             # Replay all log entries from the last checkpoint
./wal_clr undo <SEQ>       # Manually undo a specific event by its LSN sequence number
```

---

## 🧪 Testing

The engine ships with a suite of automated Python test scripts.

### 1. Core Engine & RBAC Test
Validates 13 distinct scenarios covering basic movement, shooting, role assignment (Player/Spectator), network snapshot broadcasting, and game state consistency.
```bash
python3 tests/test_engine.py
```

### 2. Concurrency, Semaphore Limit & Race Condition Stress Test
Spawns **105 independent TCP clients** simultaneously against the live server. Validates the POSIX semaphore hard limit and triggers real-time combat race conditions:
*   **Clients 1–100:** All connect successfully.
*   **Clients 101–105:** Blocked by the semaphore and timeout.
*   **Even-numbered clients** elevate to Admin and stress-test the WFG deadlock detector via rapid cross-invites and squad creations.
*   **Odd-numbered clients** remain as Players and perform high-frequency movements and shoot events to generate `[HIT]`, `[KILL]`, `[DEATH]`, `[SHIELD]`, and `[FRIENDLY FIRE]` race conditions on the terminal.

```bash
python3 tests/test_concurrency.py
```
> **Note:** Ensure `./server` is running in a separate terminal before executing either test script.
