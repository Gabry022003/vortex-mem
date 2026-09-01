<div align="center">

  <img src="assets/logo.png" alt="Vortex Memory Profiler Logo" width="600" />

  <h1>Vortex Memory Profiler</h1>
  <p>
    <strong>A high-performance, real-time memory profiling and corruption detection tool for C and C++ applications.</strong>
  </p>
</div>

## Overview

Vortex is a native memory profiling framework designed to provide granular, real-time observability into the memory lifecycle of C and C++ programs. By leveraging dynamic linkage interception and a bespoke POSIX-compliant backend, Vortex detects memory leaks, buffer overflows, and use-after-free errors while streaming live telemetry directly to a modern web dashboard.

## Why Vortex?

Developing robust systems in C and C++ demands strict memory management. Traditional profilers, while powerful, often rely on post-execution analysis or introduce significant computational overhead, making them challenging to use during interactive debugging or in latency-sensitive environments. 

Vortex was built to solve this by offering **real-time observability**. It bridges the gap between low-level system programming and modern data visualization. Instead of parsing extensive text logs after your application terminates, Vortex allows developers to monitor memory consumption, identify fragmentation, and detect anomalous behavior live, exactly as it occurs.

## Technical Architecture

Vortex is designed with a strict zero-external-dependency philosophy for its backend, relying entirely on advanced Linux OS internals and system calls to ensure minimal interference with the target application.

### 1. Zero-Interference Heap Management
To track allocations without triggering infinite recursion within the standard library's `malloc`, Vortex manages its internal state by bypassing the heap entirely. Internal data structures and hash tables are allocated directly via `mmap` with `MAP_PRIVATE | MAP_ANONYMOUS`, while callsites are retrieved through lock-free atomic chunks. This ensures that the profiler's memory footprint remains strictly isolated from the target process.

### 2. Enterprise-Grade Concurrency & Multi-Process Safety
Vortex is engineered for heavily multi-threaded and multi-processed environments, processing millions of allocations per second:
- **Striped Locking**: Thread safety is achieved through an advanced hash-based Striped Locking architecture (64 stripes aligned to CPU cache lines). This completely eliminates global mutex bottlenecks, allowing multiple threads to allocate memory simultaneously without contention.
- **Dynamic Striped Resizing**: The internal tracker expands its memory maps dynamically on a per-stripe basis. By isolating reallocation and rehashing to individual stripes, lock contention is localized, preventing global "stop-the-world" latency spikes for the target application.
- **Process Bifurcation**: The profiler gracefully handles process cloning (e.g., daemonization or worker spawning). By registering `pthread_atfork` handlers, Vortex safely suspends its internal state before a `fork()` and safely resumes in both parent and child processes, dynamically redirecting telemetry and reports to prevent file descriptor conflicts.

### 3. Integrated Telemetry Engine
Vortex eliminates the need for external runtime dependencies (like Python or Node.js) on the backend. A dedicated background thread asynchronously samples telemetry and streams real-time metrics over UDP to a highly efficient C server, which utilizes an asynchronous non-blocking event loop (`select`) to broadcast live metrics to the frontend via Server-Sent Events (SSE).

### 4. Dynamic Symbol Resolution
Raw memory addresses offer limited debugging value. Vortex automatically resolves stack traces back to their original source files and line numbers. It achieves this dynamically by combining `dladdr` to locate ELF binaries, an 8192-slot symbol cache, and direct child execution (`fork`/`exec` with sanitized environment) to invoke `addr2line` directly within the C runtime, emitting immediately readable JSON reports.

### 5. Smart Heuristic Analysis
Vortex includes an integrated diagnostic engine that analyzes memory behavior patterns:
- **Loop Leaks & Forgotten Frees**: Pinpoints allocations in loops or functions that are never deallocated.
- **Growing Containers**: Flags unbounded collection growths that are never drained.
- **Optimization Candidates**: Suggests stack buffer candidates for short-lived allocations (<5ms) and memory pool/slab candidates for frequent identical-sized buffers.

## Core Capabilities

Vortex operates in two primary modes depending on the developer's needs:

- **Performance Mode (Default)**: Optimized for speed and minimal memory footprint. It tracks allocations, identifies memory leaks, aggregates call-site statistics, and streams live telemetry.
- **Heavy Mode (Memory Safety)**: Designed for deep debugging of memory corruption.
  - **Redzones (Canaries)**: Pads allocations with precise byte signatures to immediately detect **Buffer Overflows** and **Underflows**.
  - **Poisoned Quarantine**: Replaces standard deallocation with a deferred, circular FIFO queue. Freed memory is poisoned with a recognizable byte pattern, allowing Vortex to detect fatal **Use-After-Free** violations.

## Getting Started

Vortex currently supports Linux environments and Windows via WSL (Windows Subsystem for Linux) using `LD_PRELOAD`.

### Building the Project

```bash
# Clone the repository
git clone https://github.com/Gabry022003/vortex-mem.git
cd vortex-mem

# Compile the native C backend and telemetry server
make

# Build the React frontend
cd frontend
npm install
npm run build
cd ..
```

### Running an Application

Prefix your execution command with the `vortex` script. No source code modification or special linking is required.

```bash
./vortex run ./bin/your_executable
```

The C-Server will automatically start, and your default browser will open `http://localhost:8000` to display the live memory dashboard.

### Activating Heavy Mode

To enable memory corruption detection (Redzones and Quarantine), you can simply use the `--heavy` flag:

```bash
./vortex run --heavy ./bin/your_executable
```

Alternatively, you can pass the environment variables directly:

```bash
env VORTEX_RED_ZONES=1 VORTEX_QUARANTINE=1 ./vortex run ./bin/your_executable
```

### Running Headless / CI Mode

To run in automated pipelines without launching the web server or opening a browser:

```bash
./vortex run --ci ./bin/your_executable
```

## Testing

Vortex includes an integrated, pure-C test suite that orchestrates isolated test binaries to mathematically assert that leaks, double-frees, and multithreading conditions are correctly handled.

```bash
# Compile and run the test suite
make test
```

## License & Citation

Vortex Memory Profiler is released under the **MIT License**. It is 100% free for personal, academic, and commercial use. 

If you use Vortex to profile a commercial application or within an academic research context, we kindly ask you to include an attribution by linking to this repository.
