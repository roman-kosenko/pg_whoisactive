# pg_whoisactive (pgwa)

**PostgreSQL live incident investigation tool for DBAs.**

`pg_whoisactive` is a lightweight real-time terminal monitor for PostgreSQL, inspired by SQL Server's `sp_WhoIsActive`.

It helps DBAs investigate production incidents by showing server load, PostgreSQL activity, session-level CPU and memory usage, I/O rates, query execution time, and blocking relationships — all in one terminal interface.

Author: Roman Kosenko
GitHub: https://github.com/roman-kosenko

![pg_whoisactive demo](https://github.com/roman-kosenko/pg_whoisactive/releases/download/v1.3.9/pgwa_demo.gif)

## Features

### Server overview
- Host CPU utilization
- PostgreSQL CPU utilization
- Number of CPU cores
- Transactions per second (TPS)
- Autovacuum activity
- Number of client sessions

### Session monitoring
- PID, application, user, and database
- CPU usage and CPU time
- RSS memory and memory changes
- Read and write rates
- Backend type and session state
- Wait events
- Transaction age (`XACT AGE`)
- Query age (`QUERY AGE`)
- Query execution time and SQL text
- Parallel workers displayed under their leader

### Incident investigation
- Blocking tree and blocking chains
- Detailed information about the selected session
- SQL text for the selected PID
- Comparison of transaction age and query age
- Cancel or terminate a session
- Session color highlighting for quick visual identification

## Usage

```bash
pg_whoisactive [port] [options]
pgwa [port] [options]
```

`pgwa` is a short alias created by the package installer.

### Interactive mode (default)

Starts a continuously refreshing ncurses interface. Sessions are color-coded to help identify their state at a glance.

**Keyboard controls**

| Key | Action |
|---|---|
| Up / Down | Select a row |
| `t` | Sort by execution time |
| `c` | Sort by CPU usage |
| `m` | Sort by RSS memory |
| `r` | Sort by read rate |
| `w` | Sort by write rate |
| `p` | Sort by PID |
| `f` | Filter  0:All  1:Active  2:Locks  3:User |
| `k` | Cancel or terminate the selected session, according to the implemented action; for a parallel worker, acts on its leader |
| Space | Pause / resume refresh |
| PgUp / PgDn | Scroll the query panel |
| `q` | Quit |

### Command-line options

| Option | Description |
|---|---|
| `--plain` | Continuously print plain-text output, refreshing every second |
| `--snapshot`, `-s` | Print one snapshot and exit; suitable for scripts, cron jobs, or logging |
| `--version`, `-v` | Show the version and exit |
| `--help`, `-h` | Show help |

### Arguments

- `port` — PostgreSQL port. Defaults to `5432`.

### Examples

```bash
# Start the interactive monitor
sudo -u postgres pg_whoisactive

# Connect to a PostgreSQL instance on port 5433
sudo -u postgres pgwa 5433

# Continuously print plain-text output
pg_whoisactive --plain

# Append a snapshot to a log
sudo -u postgres pgwa --snapshot >> /tmp/pgwa.log
```

## Metrics explained

The tool combines PostgreSQL statistics with operating-system process metrics. This helps distinguish database activity from resource consumption by individual backend processes.

### Server-level metrics

| Metric | What it means | How to interpret it |
|---|---|---|
| **Host CPU** | CPU utilization across the whole machine | High usage indicates that the host is busy. Check whether PostgreSQL or other processes are consuming the CPU. |
| **PG CPU** | Combined CPU usage of PostgreSQL backend processes, capped at Host CPU | Shows how much of the host's CPU activity is attributable to PostgreSQL processes tracked by the tool. |
| **CPUs** | Number of CPU cores available to the host | Provides context for interpreting CPU utilization. |
| **TPS** | Transactions per second, calculated from commits and rollbacks across databases | A change in TPS helps identify changes in transaction throughput. High TPS alone does not indicate a problem. |
| **AV** | Number of currently running autovacuum workers relative to `autovacuum_max_workers` | For example, `2/6` means two workers are running out of a configured maximum of six. |
| **Sessions** | Number of client sessions | Helps identify unusually high connection counts. Parallel workers are displayed separately beneath their leader. |

### Per-session and per-process metrics

| Metric | What it means | How to interpret it |
|---|---|---|
| **CPU%** | CPU consumed by a process during the last sampling interval, expressed relative to the total capacity of all CPU cores | A value of `100%` represents one fully utilized core. On a multi-core host, a process can use a fraction of total machine capacity even while fully occupying one core. |
| **CPUms** | CPU time consumed by the process during the last sampling interval, in milliseconds | Useful for seeing how much actual processor time a process consumed, regardless of how much wall-clock time passed. |
| **READ/s** | Read bytes per second reported by the operating system for the process | Helps identify processes with high read activity. These counters do not necessarily represent physical disk reads; cached and other I/O behavior can affect interpretation. |
| **WRITE/s** | Write bytes per second reported by the operating system for the process | Helps identify processes generating write activity. The counter does not by itself prove that the data was synchronously written to physical storage. |
| **RSS** | Resident Set Size: the process's current resident memory, reported by the operating system | Shows how much physical memory is resident for the process. RSS values for multiple processes can overlap because shared memory may be counted in more than one process. |
| **dRSS** | Change in RSS since the previous sample | Positive values indicate an increase in resident memory; negative values indicate a decrease. It is a change in RSS, not a direct measurement of a memory leak. |

### Session state and timing

| Field | What it means | Why it matters |
|---|---|---|
| **State** | PostgreSQL session state, such as active or idle | Helps distinguish sessions executing a query from sessions that are not currently executing one. |
| **Wait event** | The event on which a backend is waiting, when applicable | Helps investigate lock waits and other sources of waiting. |
| **XACT AGE** | Age of the current transaction | An old transaction can retain resources and interfere with maintenance or cleanup. An idle-in-transaction session deserves particular attention. |
| **QUERY AGE** | Time elapsed since the current query started, where applicable | Helps identify queries that have been running for a long time. |
| **Execution time** | Query duration as presented by the tool | Helps compare query runtimes and prioritize investigation. Interpret it together with session state and query age. |

**Important distinction:** `XACT AGE` and `QUERY AGE` measure different things. A transaction can remain open across multiple queries, so transaction age can be much greater than the age of the current query.

### How the metrics are calculated

- **CPU%:** based on the change in process CPU ticks (`utime + stime`) read from `/proc/<pid>/stat` between samples.
- **CPUms:** converts the change in CPU ticks into milliseconds using the system's clock-tick frequency.
- **READ/s and WRITE/s:** calculated from changes in the process I/O counters in `/proc/<pid>/io`, divided by the elapsed sampling time.
- **RSS:** read from `VmRSS` in `/proc/<pid>/status`.
- **dRSS:** current RSS minus the previous RSS sample.
- **TPS:** calculated from the change in `xact_commit + xact_rollback` across databases, excluding `template0` and `template1`.
- **PG CPU:** aggregated from the CPU measurements of PostgreSQL processes tracked by the tool, with a cap at Host CPU.

Rates and deltas require at least two samples. In interactive and `--plain` modes, these values become available after the second refresh. The `--snapshot` mode collects two samples internally before printing its result.

Parallel workers are separate operating-system processes, so their CPU, memory, and I/O metrics are read using their own PIDs. They are displayed beneath their leader to make parallel query activity easier to understand.

## Color coding

- **Green** — active session.
- **Yellow** — session waiting on a lock.
- **Red** — aborted transaction.
- **Brighter green** — parallel worker.

Colors provide a quick visual cue; use the session state, wait event, and query details to understand the actual cause of a problem.

## Requirements and permissions

- PostgreSQL accessible through a local Unix socket by default.
- The specified PostgreSQL port must be available if connecting to a non-default instance.
- Permission to read the `/proc` information of PostgreSQL backend processes is required for CPU, I/O, and memory metrics.
- Running as the operating-system `postgres` user is recommended for local socket authentication without a password, provided PostgreSQL authentication rules permit it.
- PostgreSQL 13 or later
- Linux with /proc filesystem
- libpq and ncurses

## Typical investigation workflow

1. **Check the server overview.** Determine whether the host or PostgreSQL is under heavy CPU load and inspect TPS and autovacuum activity.
2. **Find resource-intensive sessions.** Sort by CPU, RSS, read rate, write rate, or execution time.
3. **Investigate waits and blocking.** Examine wait events, blocking chains, and the selected session's details.
4. **Check transaction and query age.** A long-running query and a long-lived transaction are not necessarily the same problem.
5. **Inspect the SQL text.** Determine what the selected backend is executing.
6. **Take action carefully.** Cancel or terminate a session only after assessing its role and impact.

`pg_whoisactive` is an investigation aid: it brings important diagnostic signals together in one place so that DBAs can quickly decide where to look next.

## Third-party libraries

This project uses the following third-party libraries:

libpq — PostgreSQL License

ncurses — primarily distributed under an MIT/X11-style license

These libraries are subject to their respective licenses. Their copyright notices and license terms remain applicable.

## Author and copyright

Copyright (c) 2026 Roman Kosenko.

## License

pg_whoisactive is licensed under the GNU General Public License v3.0 (GPL-3.0).

See the LICENSE file for the full license text.
