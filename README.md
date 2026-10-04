# ProcessPilot Pro

## Dependency-Aware Linux Service Supervisor

ProcessPilot Pro is a lightweight Linux service supervisor written in
C++17. It reads service definitions and dependencies from an INI-style
configuration, starts services in dependency order, monitors crashes,
and restarts services according to configurable policies. Runtime
control is provided by `ppctl` over a Unix domain socket.

## Features

-   INI configuration with validation
-   Unknown and cyclic dependency detection
-   Dependency-ordered startup and reverse-order stopping
-   Crash detection using `SIGCHLD` / `waitpid`
-   `never`, `on-failure`, and `always` restart policies
-   Exponential backoff and maximum restart limits
-   Restart cascades for dependent services
-   Graceful shutdown with timeout handling
-   Runtime control through `ppctl`
-   Per-service log files

## Requirements

-   Linux (Ubuntu 20.04+ recommended)
-   `g++` 9 or newer with C++17 support
-   `make`, `bash`, and `git`
-   Windows users can use WSL2 or a Linux VM
-   macOS is not supported
-   No root privileges or additional libraries are required

Ubuntu setup:

``` bash
sudo apt update
sudo apt install -y build-essential git
```

## Build

``` bash
git clone https://github.com/sunnyTechgit/processpilot-project.git
cd processpilot-project
make
```

This creates `build/processpilot` and `build/ppctl`.

## Tests

``` bash
make test
```

Expected result:

``` text
19/19 unit checks passed
integration: 24 passed, 0 failed
```

## Demo

Start the supervisor:

``` bash
./build/processpilot -c examples/demo.conf -l /tmp/pp-logs
```

In another terminal:

``` bash
./build/ppctl status
```

You can test restart and dependency behavior with:

``` bash
kill -9 <pid of db>
./build/ppctl status
./build/ppctl stop db
./build/ppctl start web
./build/ppctl logs 20
./build/ppctl shutdown
```

Validate a configuration without running the supervisor:

``` bash
./build/processpilot --check -c examples/demo.conf
```

## Configuration

Example:

``` ini
[db]
command=sleep 100000
restart=always

[api]
command=sleep 100001
depends=db
restart=on-failure
max_restarts=5
backoff_ms=500
```

Supported restart policies are `never`, `on-failure`, and `always`.

## Commands

``` text
status
start <svc|all>
stop <svc|all>
restart <svc|all>
logs [n]
shutdown
```

`start X` also starts X's dependencies. `stop X` also stops X's
dependents.

## Project Structure

``` text
processpilot-project/
├── src/
├── tests/
├── examples/
└── Makefile
```

`src/` contains the configuration parser, dependency graph, supervisor
core, daemon main, and `ppctl` client. `tests/` contains unit and
integration tests. `examples/` contains `demo.conf`.

## Architecture

``` text
config file -> parser -> dependency graph -> supervisor event loop
                                             |-- fork/exec services
                                             |-- SIGCHLD -> waitpid
                                             |-- restart/backoff/cascade
                                             '-- Unix socket <-> ppctl
```

The supervisor uses a single-threaded `poll()` loop with `signalfd`, the
control socket, and a 100 ms tick. Service states include `Stopped`,
`Running`, `Backoff`, `Stopping`, and `Failed`.

## Limitations and Future Work

There is currently no cgroup/namespace isolation, no configuration
reload, readiness is time-based rather than health-probe based, and the
project is single-host.

Possible future additions include health checks, configuration reload,
cgroup limits, and a kernel-module event sink.

ProcessPilot is a user-space supervisor and does not require a kernel
driver.
