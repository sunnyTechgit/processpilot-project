# ProcessPilot Pro — Dependency-Aware Linux Service Supervisor (C++17)

A small init-style supervisor: reads a config of services and dependencies, starts them in
topological order, detects crashes via `SIGCHLD`/`waitpid`, restarts with exponential backoff,
cascades restarts to dependents, and is controlled at runtime with `ppctl` over a Unix socket.

## Build & run
    make                      # builds build/processpilot and build/ppctl
    make test                 # unit tests + end-to-end integration test
    ./build/processpilot -c examples/demo.conf -l /tmp/pp-logs
    ./build/ppctl status      # in another terminal
    ./build/ppctl restart db  # also bounces api and web
    ./build/ppctl stop all ; ./build/ppctl shutdown

## Config (INI)
    [name]            # or [service:name]
    command=...       # required, quotes supported, no shell
    depends=a, b
    restart=never|on-failure|always     (default on-failure)
    max_restarts=5  backoff_ms=500  ready_delay_ms=0  stop_timeout_ms=3000

## Commands
`status`, `start <svc|all>`, `stop <svc|all>`, `restart <svc|all>`, `logs [n]`, `shutdown`.
`start X` also starts X's dependencies; `stop X` also stops X's dependents.


Docs for each project stage are in `docs/`.
