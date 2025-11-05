# 🚀 Advanced IPC Chat Server in C

**An Operating Systems project (ACS, KMUTT)** demonstrating advanced **Inter-Process Communication (IPC)** and **Concurrency Management** in pure C.

This project re-engineers the traditional single-loop chat server into a **high-performance multi-threaded architecture**, adopting a **Router–Worker (Producer–Consumer)** model for massive concurrent message handling.

---

## ⚙️ Core Technologies

- **System V Message Queues:** `msgget`, `msgsnd`, `msgrcv`  
- **POSIX Threads (Pthreads):** Multithreading primitives  
- **Reader–Writer Locks:** `pthread_rwlock_t` for efficient concurrent access  
- **Mutexes & Condition Variables:** `pthread_mutex_t`, `pthread_cond_t` for job queue synchronization  

---

## 🏛️ Architecture Overview

This upgraded architecture eliminates **bottlenecks** and **race conditions** found in earlier versions.

### 1. Client IPC — `IPC_PRIVATE` Queues (PID-safe)
Each client now creates a **private reply queue** using `msgget(IPC_PRIVATE, ...)`, ensuring a unique queue ID system-wide.

**Flow:**
1. The client sends `CMD_REGISTER` with its `reply_qid` to the server.  
2. The server stores this `reply_qid` in the `GlobalRegistry`.  
3. Replies are sent directly to each client’s private queue.

---

### 2. Server Architecture — Router–Worker Pattern

The server consists of **three major components** running on **multiple threads**:

#### 🧭 Router Thread
- Listens for commands on the **CONTROL_QUEUE_KEY**.  
- Parses incoming `CommandMessage` objects.  
- Updates client `last_active` status.  
- Dispatches lightweight "jobs" into the shared `Job Queue`.  

*Optimized for speed — no blocking I/O.*

#### 📡 Broadcaster Pool
- A pool of worker threads (size defined by `BROADCASTER_COUNT`).  
- Continuously fetches jobs using `get_job()`.  
- Performs actual message broadcasting via `msgsnd()` to all relevant clients.  
- Uses `IPC_NOWAIT` to prevent one slow client from stalling the system.  

#### 🕵️‍♂️ Monitor Thread
- Runs every 10 seconds to check `last_active`.  
- Removes inactive clients exceeding `INACTIVITY_TIMEOUT`.

---

### 3. Data Flow Diagram
Client → [Control Queue] → (Router)
↓
[Job Queue] → (Broadcaster Pool)
↓
[Reply Queues] → Clients

---

## 🔒 Concurrency Model

### 🗂 GlobalRegistry
- Stores all client and room states.  
- Protected by **Reader–Writer Locks** (`pthread_rwlock_t`).  
- Allows concurrent **reads** but exclusive **writes**, providing better throughput than a standard mutex.

### 🧾 Job Queue
- Shared between Router (producer) and Broadcasters (consumers).  
- Implemented as a **thread-safe linked list**.  
- Guarded by `pthread_mutex_t` and `pthread_cond_t` for signaling when new jobs arrive.

---

## 📁 File Structure

| File | Description |
|------|--------------|
| `project_defs.h` | Shared “contract” between server and client — defines enums, structs, and message formats |
| `main.c` | Server logic (Router, Broadcaster Pool, Monitor) |
| `client.c` | Client-side logic (sending commands, receiving messages) |

### Breakdown

#### `main.c` (Server)
- `main()`: Initializes message queues, RW locks, and threads.  
- `router_thread()`: Receives commands and dispatches jobs.  
- `broadcaster_thread()`: Executes jobs and sends messages.  
- `monitor_clients()`: Cleans up inactive users.  

#### `client.c` (Client)
- `main()`: Creates `reply_qid` and connects to server queue.  
- `sender_thread()`: Reads user input, parses commands, and sends requests.  
- `receiver_thread()`: Waits for `ReplyMessage` via private queue.  
- `cleanup()`: Removes the private queue before exit.

---

## 🛠️ Installation & Usage (with Docker)

### 1. Build Docker Image
Place `Dockerfile`, `main.c`, `client.c`, and `project_defs.h` in the same directory.

```bash
docker build -t my-chat-app .
```
(Dockerfile example should include: COPY . . and CMD ["sleep", "infinity"])

### 2. Run Container (Detached)
```bash
docker run -d --name chat_container my-chat-app
```

### 3. Compile & Run Server
```bash
# Compile inside container
docker exec chat_container gcc main.c -o server -lpthread

# Run server in background
docker exec -d chat_container /app/server
```

### 4. Run Client(s)
Open a new terminal for each client instance.
```bash
# Compile client
docker exec chat_container gcc client.c -o client -lpthread

# Run interactively
docker exec -it chat_container /app/client
```

### 5. Cleanup
```bash
docker stop chat_container && docker rm chat_container
```