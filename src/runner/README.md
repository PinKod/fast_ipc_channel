# args
runner "path to data file" "path to producer" "path to consumer" "cpu cores(2) sep with ','" "shm name"

The runner itself is not pinned to any core; the two cores given are the producer's and
consumer's own core, respectively. Each of producer/consumer pins its main thread to the
core it's given, and may pin a second/helper thread to `core + 1` (via `cpu_utils.h`'s
`pin_current_thread`/`get_current_cpu_core`), so pick core ids that leave each pair's
`core`/`core + 1` mapped to independent physical cores (check `lstopo` for SMT sibling pairs).

## prod/cons args
producer/consumer "shm name" "path to data file" "cpu core id"



## flowchart:

```mermaid
flowchart TD
    A(["START: main()"]) --> B["Call validate_args()"]
    B --> C{" argc == 6?"}
    C -->|No| D["Return 1"]
    C -->|Yes| E["Check argv[1] file exists"]
    E --> F{" File exists?"}
    F -->|No| D
    F -->|Yes| G{" File size > 0?"}
    G -->|No| D
    G -->|Yes| H["check_exec(argv[2], PROD)"]
    H --> I{" Valid executable?"}
    I -->|No| D
    I -->|Yes| J["check_exec(argv[3], CONS)"]
    J --> K{" Valid executable?"}
    K -->|No| D
    K -->|Yes| L["check_cpu_cores(argv[4])"]
    L --> M{" Parse 2 cores?"}
    M -->|No| D
    M -->|Yes| N{" Cores unique?"}
    N -->|No| D
    N -->|Yes| O{" Cores valid?"}
    O -->|No| D
    O -->|Yes| P{" argv[5] empty?"}
    P -->|Yes| D
    P -->|No| Q{" argv[5] contains '/'?"}
    Q -->|Yes| D
    Q -->|No| R{" shm_name already exists?"}
    R -->|Yes| D
    R -->|No| S["Return 0"]
    S --> W["Call spanw_procs()"]
    W --> X["Initialize barrier"]
    X --> Y{" Barrier init ok?"}
    Y -->|No| Z["Return 1"]
    Y -->|Yes| AA["Create pthr_argv args1"]
    AA --> AB["Create pthr_argv args2"]
    AB --> AC["pthread_create t1"]
    AC --> AD{" Thread 1 created?"}
    AD -->|No| AE["Clean up, Return 1"]
    AD -->|Yes| AF["pthread_create t2"]
    AF --> AG{" Thread 2 created?"}
    AG -->|No| AH["Cancel t1, Clean up, Return 1"]
    AG -->|Yes| AI["pthread_join t1"]
    AI --> AJ["pthread_join t2"]
    AJ --> AK["Print results"]
    AK --> AL["Return 0"]
    
    subgraph "run_proc() - Thread Function"
        AN["fork() child process"]
        AO{" Fork ok?"}
        AP["Send SIGSTOP to child"]
        AQ["execl() child program"]
        AR["waitpid WUNTRACED"]
        AS["pthread_barrier_wait"]
        AT["Get start time"]
        AU["kill(SIGCONT)"]
        AV["waitpid() exit"]
        AW["Get end time"]
        AX["Calculate elapsed_time"]
        AY["Print results"]
        AZ["Return NULL"]
    end
    
    AN --> AO
    AO -->|Error| AZ
    AO -->|Success| AP
    AP --> AQ
    AQ --> AR
    AR --> AS
    AS --> AT
    AT --> AU
    AU --> AV
    AV --> AW
    AW --> AX
    AX --> AY
    AY --> AZ
    
    AL --> BA(["END: Success"])
    D --> BB(["END: Error"])
    Z --> BB
    AE --> BB
    AH --> BB
```
