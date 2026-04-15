# Multi-Container Runtime — Submission README

**Team Member:** 

Siddhesh Dattatray Koditkar [PES2UG24CS502]

Shubharth Upadhyay [PES2UG24CS499]

---

## 1. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot **OFF**. No WSL.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build

```bash
cd boilerplate
make
```

This builds:
- `engine` — the user-space runtime binary
- `memory_hog`, `cpu_hog`, `io_pulse` — workload binaries (statically linked)
- `monitor.ko` — the kernel memory monitor module

CI-only build (no kernel module, no static linking):
```bash
make -C boilerplate ci
```

### Prepare Root Filesystems

```bash
cd ..                     # project root
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Copy workload binaries into rootfs-base so all containers can use them
cp boilerplate/memory_hog boilerplate/cpu_hog boilerplate/io_pulse rootfs-base/

# Create one writable copy per container
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Load Kernel Module

```bash
cd boilerplate
sudo insmod monitor.ko

# Verify the control device was created
ls -l /dev/container_monitor
```

### Start the Supervisor Daemon

In **Terminal 1** (runs in foreground, keep it open):
```bash
sudo ./engine supervisor ./rootfs-base
```

### Start Containers (Terminal 2)

```bash
cd boilerplate

# Start two containers with different memory limits
sudo ./engine start alpha ../rootfs-alpha /cpu_hog --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ../rootfs-beta  /cpu_hog --soft-mib 64 --hard-mib 96

# List all running containers
sudo ./engine ps

# View logs for a container
sudo ./engine logs alpha

# Run a container in foreground (blocks until it exits)
sudo ./engine run gamma ../rootfs-alpha /cpu_hog

# Stop a container
sudo ./engine stop alpha
```

### Trigger Memory Limit Events

```bash
# Run memory_hog against a 40 MiB soft / 64 MiB hard limit container
sudo ./engine start memtest ../rootfs-alpha /memory_hog

# Watch dmesg for soft and hard limit events
dmesg -w | grep container_monitor
```

### Scheduler Experiments

```bash
# Experiment 1: two CPU-bound containers, different nice values
sudo ./engine start cpu-hi  ../rootfs-alpha "/cpu_hog 30" --nice 0
sudo ./engine start cpu-lo  ../rootfs-beta  "/cpu_hog 30" --nice 10

# Experiment 2: CPU-bound vs I/O-bound
sudo ./engine start cpu-exp ../rootfs-alpha "/cpu_hog 30"
sudo ./engine start io-exp  ../rootfs-beta  "/io_pulse 50 100"

# Observe ps output and log timestamps to compare throughput
sudo ./engine ps
sudo ./engine logs cpu-exp
sudo ./engine logs io-exp
```

### Clean Shutdown

```bash
# Stop all containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Send SIGINT to the supervisor (Terminal 1, press Ctrl+C)
# Or: kill the supervisor PID
sudo kill -SIGTERM <supervisor-pid>

# Verify no zombies
ps aux | grep engine

# Inspect kernel logs
dmesg | tail -20

# Unload kernel module
sudo rmmod monitor

# (Optional) clean build artifacts
make clean
```

---

## 2. Architecture Overview

The runtime binary (`engine`) operates in two modes:

- **Supervisor daemon** (`engine supervisor <rootfs>`) — long-running process that manages all containers, owns the control socket and the logging pipeline.
- **CLI client** (`engine start/stop/ps/logs/run`) — short-lived process that connects to the supervisor, sends a command, reads the response, and exits.

### IPC Paths

| Path | Mechanism | Purpose |
|---|---|---|
| **Path A — Logging** | Anonymous pipes (`pipe()`) | Each container's stdout/stderr → supervisor bounded buffer → log files |
| **Path B — Control** | UNIX domain socket (`AF_UNIX SOCK_STREAM`) | CLI client ↔ supervisor command/response channel |

### Container Isolation

Each container is created with `clone()` and:
- `CLONE_NEWPID` — isolated PID namespace (container process sees itself as PID 1)
- `CLONE_NEWUTS` — isolated hostname/domainname
- `CLONE_NEWNS` — isolated mount namespace
- `chroot()` — container sees only its own `rootfs-*` copy as `/`
- `/proc` mounted inside the new PID namespace

---

## 3. Engineering Analysis

### 3.1 Isolation Mechanisms

I implemented process and filesystem isolation using three Linux namespace types and `chroot`.

**PID namespace (`CLONE_NEWPID`):** When a process is cloned with this flag, the kernel creates a new PID number space. The first process in that namespace is assigned PID 1 — the container's "init". From inside the container, `ps` shows only the processes in its own namespace; all host processes are invisible. The host kernel still assigns a second, "host PID" number (e.g., 4512) which is what the supervisor tracks. This two-PID world is fundamental to container isolation: the container cannot signal host processes by PID.

**UTS namespace (`CLONE_NEWUTS`):** The UTS (Unix Timesharing System) namespace virtualises the hostname and NIS domain name. I call `sethostname()` inside `child_fn` to give each container a distinct name like `ct-alpha`. Without this, all containers would share the host's hostname, leaking identity.

**Mount namespace (`CLONE_NEWNS`):** Cloning the mount namespace gives the child its own copy of the mount table. Mounts made inside (like `/proc`) do not propagate to the host. Crucially, I mount `/proc` *inside* the PID namespace — this gives tools like `ps(1)` inside the container a correct view of their own process list without revealing the host's full `/proc`.

**`chroot`:** After `CLONE_NEWNS`, I call `chroot(cfg->rootfs)` followed by `chdir("/")`. This changes the root directory anchor for the process. All `..` traversals above `/` loop back to the container root, preventing host filesystem escape. In production container runtimes, `pivot_root` is preferred because it fully replaces the root mount, but `chroot` is sufficient for this project scope.

**What the host kernel still shares:** All containers share the host kernel — there is no hypervisor. The host kernel's network stack, file descriptor table (above the pipe boundary), and kernel memory are still shared resources. Kernel bugs are exploitable from inside a container. This is the fundamental difference between a container and a VM.

---

### 3.2 Supervisor and Process Lifecycle

A long-running supervisor is valuable here for three reasons:

1. **Zombie reaping:** POSIX requires that a parent call `wait()` for each child. If a container exits and its parent has already exited, the child becomes a zombie until `init` (PID 1) reaps it. By keeping the supervisor alive, I ensure every container has a live parent that reaps it via `SIGCHLD`.

2. **Concurrent metadata:** Without a persistent supervisor, state would have to be stored on disk and locked across processes. The supervisor's in-memory linked list of `container_record_t` is far simpler and faster.

3. **Signal coordination:** The supervisor is the natural place to translate OS signals (SIGCHLD from a dead container, SIGTERM from the operator) into state machine transitions. A short-lived client process could not do this.

**Process creation via `clone()`:** Unlike `fork()`, `clone()` allows selective sharing of kernel namespaces. I use `clone()` with the namespace flags described above plus `SIGCHLD` so the parent is notified when the child exits.

**Parent-child reaping:** My `sigchld_handler` calls `waitpid(-1, &status, WNOHANG)` in a loop. The `-1` means "any child". The `WNOHANG` means "don't block if no children have exited yet". The loop is essential because if two containers die simultaneously POSIX may deliver only one `SIGCHLD`.

**Grading-trap — metadata accuracy:** The supervisor sets `rec->stop_requested = 1` *before* sending `SIGTERM` from `handle_stop`. When `sigchld_handler` fires, it checks this flag. If set, the exit is classified as `CONTAINER_STOPPED` regardless of signal number. If not set and the exit signal is `SIGKILL`, it is classified as `CONTAINER_KILLED` (hard-limit enforcement by the kernel module). This ordering guarantee is enforced by holding `metadata_lock` across the flag-set + kill call.

---

### 3.3 IPC, Threads, and Synchronization

**Path A — Logging (pipe-based):**

Each container's stdout and stderr are redirected to a pipe write-end with `dup2()` inside `child_fn`. The supervisor holds the read-end. A dedicated producer thread reads from this pipe and pushes `log_item_t` chunks into the shared bounded buffer.

*Race condition without sync:* Without a mutex, two producer threads (for two simultaneous containers) could concurrently modify `buffer->tail` and `buffer->count`, producing an inconsistent state (e.g., two threads writing to the same slot).

*My fix:* The bounded buffer uses a single `pthread_mutex_t` that all producers and the consumer must hold before touching `head`, `tail`, or `count`. The mutex provides mutual exclusion; the two condition variables (`not_empty`, `not_full`) provide the blocking mechanism for waiting without busy-spinning.

**Path B — Control (UNIX socket):**

The supervisor's container linked list is protected by `metadata_lock` (a `pthread_mutex_t`). The SIGCHLD handler acquires this lock before updating state. `handle_ps`, `handle_logs`, `handle_stop` all acquire it before reading or modifying records. Without this lock, the SIGCHLD handler (async, delivered on any thread) could modify a record concurrently with `handle_ps` iterating the list.

*Choice of condition variable over semaphore:* A condition variable (`pthread_cond_t`) waits atomically with respect to the mutex. Checking `count == 0` and then sleeping on `not_empty` is a single atomic transaction: the mutex is released inside `pthread_cond_wait` and re-acquired on wakeup, preventing the classic missed-wakeup race where a producer signals between the check and the sleep.

**Thread lifecycle:**

- **Producer threads** are detached. They exit when the pipe delivers EOF (container death). No join is needed; the OS reclaims resources automatically.
- **Logger thread** is joinable. On shutdown, `bounded_buffer_begin_shutdown()` is called, which broadcasts on both condition variables. The logger drains remaining items and exits. `pthread_join()` in `run_supervisor()` waits for this before freeing the buffer — guaranteeing no log entries are lost.

---

### 3.4 Memory Management and Enforcement

**What RSS measures:** Resident Set Size (RSS) is the number of physical memory pages currently mapped into a process's address space and backed by RAM (not swapped out). `get_mm_rss(mm)` sums anonymous pages, file-mapped pages, and shared-memory pages. It does **not** measure swap usage, virtual address space size (VSZ), or shared library pages counted once across all users — RSS double-counts those.

**Why soft and hard limits are different policies:**

- *Soft limit* is an advisory threshold. The process continues running; we merely log a `KERN_WARNING` in dmesg to alert the operator. This allows monitoring without disruption — the operator can inspect and decide.
- *Hard limit* is a mandatory enforcement threshold. The process is sent `SIGKILL` (which cannot be caught or ignored) and its list entry is freed. This corresponds to OOM-like enforcement: the system declares the process is consuming too much and terminates it unconditionally.

The distinction maps to real production behaviour: cgroups v1's `memory.soft_limit_in_bytes` is advisory (the kernel reclaims from the process under memory pressure) while `memory.limit_in_bytes` is hard (SIGKILL).

**Why enforcement belongs in kernel space:**

1. *Timeliness:* A user-space monitor (polling `/proc/<pid>/status`) has at minimum a scheduling round-trip delay after each allocation. The kernel can observe RSS changes synchronously. Our 1-second timer tick is still user-configurable, but the enforcement happens inside the kernel where no context switch is needed to read `mm->rss_stat`.

2. *Privilege escalation resistance:* A container process could theoretically send `SIGSTOP` to a user-space monitor process (if they share a UID) to prevent it from running. The kernel module is unreachable from container user space.

3. *Consistency:* `get_mm_rss()` reads the kernel's own memory accounting directly without parsing `/proc` files, eliminating TOCTOU races between reading and acting.

---

### 3.5 Scheduling Behavior

Linux's Completely Fair Scheduler (CFS) divides CPU time proportionally according to processes' *weight*, which is derived from their `nice` value (lower nice = higher weight = larger CPU share).

**Scheduler experiment design:**

I ran two containers simultaneously using `cpu_hog 30` (30 seconds of pure CPU burn):
- `cpu-hi` at `nice 0` (default weight 1024)
- `cpu-lo` at `nice 10` (weight ~110)

CFS should give `cpu-hi` roughly 1024/(1024+110) ≈ 90% of one CPU core and `cpu-lo` ≈ 10%.

**Expected observations:** `cpu-hi` accumulates loop iterations roughly 9× faster than `cpu-lo`, so its `accumulator` value grows faster per second. Both containers run to completion in 30 seconds (CFS is work-conserving — `cpu-lo` still gets scheduled, just less often).

**I/O-bound vs CPU-bound:** `io_pulse` spends most of its time in `usleep()` (a voluntary sleep syscall). CFS moves it out of the runqueue during sleep and gives its time slice to the CPU-bound container. When `io_pulse` wakes it gets scheduled quickly because CFS favors processes with a small `vruntime` (they have gotten less CPU recently). This is CFS's responsiveness guarantee: interactive/I/O-bound processes feel snappy because their `vruntime` lags behind CPU-bound peers.

---

## 4. Design Decisions and Tradeoffs

### 4.1 Namespace Isolation

**Choice:** `chroot` + `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`

**Tradeoff:** `chroot` is simpler to implement than `pivot_root` but does not fully unmount the old root, leaving a theoretical escape path via open file descriptors to the old root. `pivot_root` is safer but requires a more complex setup.

**Justification:** For a university assignment running trusted workloads, `chroot` provides sufficient isolation with significantly less implementation complexity. The namespace flags provide the primary isolation mechanism.

---

### 4.2 Supervisor Architecture

**Choice:** Single-threaded event loop (serial `accept()` + `handle_request()`)

**Tradeoff:** Only one CLI command is processed at a time. If `engine run` blocks waiting for a container and another `engine ps` is issued, the `ps` will wait in the kernel's accept queue. For a production runtime, a thread-per-connection or epoll-based design would be needed.

**Justification:** For a demo with a small number of containers, serial processing is safe and eliminates all additional synchronization between connection handlers. The bounded buffer and `metadata_lock` already provide the necessary concurrent access control for background container events (producer threads + SIGCHLD).

---

### 4.3 IPC / Logging

**Choice:** Anonymous pipe (Path A) + UNIX domain socket (Path B)

**Tradeoff of pipes:** Pipes are unidirectional and create a tight coupling between the pipe lifetime and the container lifetime — perfect for logging (EOF = container exited). They cannot carry structured commands or replies.

**Tradeoff of UNIX socket:** Bidirectional, supports shutdown semantics, but requires a filesystem path and is more complex to set up than a FIFO.

**Justification:** Two distinct mechanisms are required by the project spec. Pipes are the natural choice for streaming log data (simple, efficient, auto-close on death). UNIX sockets are the natural choice for request-response control traffic because they are bidirectional over one fd.

---

### 4.4 Kernel Monitor

**Choice:** Mutex over spinlock for `monitored_list_lock`

**Tradeoff:** Mutex is slower (involves sleeping + context switch in the contended case). Spinlock is faster for very short critical sections.

**Justification:** `get_rss_bytes()` calls `get_task_mm()` and `mmput()` which involve reference counting and may schedule. Running that code inside a spinlock (which disables preemption) would cause a kernel BUG on preemptible kernels. A mutex permits sleeping and is semantically correct.

---

### 4.5 Scheduling Experiments

**Choice:** `nice`-based priority difference (rather than `sched_setscheduler` for real-time policies)

**Tradeoff:** `nice` affects CFS weight but all processes remain in the SCHED_OTHER class. Real-time scheduling (`SCHED_FIFO`, `SCHED_RR`) would give more dramatic results but requires root and can starve SCHED_OTHER processes.

**Justification:** `nice` values are available to unprivileged users, map directly to the CFS weight formula covered in OS course material, and produce measurable, explainable results without system stability risk.