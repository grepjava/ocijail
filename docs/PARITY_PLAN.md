# ocijail → crun parity plan

## Reference runtime: crun

The plan measures ocijail against **[crun](https://github.com/containers/crun)**.
runc is the fallback reference where crun leaves something unspecified.

Why crun and not runc:

- **It is podman's default on Linux.** podman and buildah are ocijail's only
  real consumers, so "parity" in practice means "podman can't tell the
  difference." crun's CLI and its error and exit-code behaviour are what
  podman is tested against.
- **ocijail already tracks it.** `delete` explicitly copies crun's stricter
  semantics ([delete.cpp](../ocijail/delete.cpp)), and `features` follows
  the crun/runc output format.
- **The scale is similar.** crun is a small native runtime with no Go
  runtime and no daemon. Its design choices (fork/exec sync over socket
  pairs, a fifo for start) carry over to C++ and jails more directly than
  runc's `libcontainer` and `nsexec` do.

Parity means **matching behaviour, not mechanism**. Where Linux has a kernel
feature FreeBSD lacks, the target is the closest FreeBSD primitive (rctl,
procctl, devfs rules, jail params). When there is no such primitive, the
runtime refuses the request with a clear error and reports it accurately in
`features`.

## Where ocijail stands (updated 2026-10-26)

"main" is upstream ocijail v0.7.0-dev. "With branches" is the state with this plan's branches applied. None of them is merged yet; see Progress below for the branch and PR behind each one.

| Area | crun | main | With branches |
|---|---|---|---|
| `create` `start` `state` `delete` `kill` `list` `exec` `features` | ✅ | ⚠️ correctness bugs | ✅ Phase 0 bugs fixed (PRs #26–#32) |
| `run` | ✅ | ❌ | ✅ `run` |
| `ps` | ✅ | ❌ | ✅ `ps` |
| `spec` | ✅ | ❌ | ✅ `spec`; `freebsd` section in `spec-freebsd` |
| `state` fields `created` `owner` `rootfs` | ✅ | ❌ | ✅ `state-fields` |
| Global `--debug` `--systemd-cgroup` `--cgroup-manager` `--rootless` | ✅ | ❌ | ✅ `global-flags` |
| `exec` command as arguments | ✅ | ❌ (only `--process`) | ✅ `exec-argv` |
| `exec` flags `--cwd -e --user` | ✅ | ❌ | ✅ `exec-flags` |
| `exec --no-new-privs` | ✅ | ❌ | ✅ `exec-no-new-privs` (on `exec-flags` + `no-new-privs`) |
| `exec --cap` `--process-label` | ✅ | ❌ | ➖ not added (no capabilities or labels on FreeBSD) |
| `kill --all` | ✅ | ⚠️ parsed, then ignored | ✅ PR #26 |
| `kill --pid` | ✅ | ❌ | ✅ PR #33 |
| `list -q` | ✅ | ⚠️ parsed, then ignored | ✅ PR #27 |
| Hook timing | per spec | ⚠️ `prestart` in `start`, `poststart` doesn't wait for exec | ✅ `prestart-in-create`, `poststart-after-exec` |
| Hook failures and `timeout` | per spec | ❌ exit status ignored | ✅ `hook-errors` |
| Hook `args` as argv | per spec | ⚠️ path prepended | ✅ PR #36 |
| Stale state (jail gone, jid/pid reused) | ✅ | ⚠️ acts on unrelated jail/process | ✅ PR #37 |
| `creating` status; cleanup after a failed `create` | ✅ | ⚠️ leaks mounts | ✅ PR #38 |
| Exit status recording | ✅ (`run` only) | ❌ | ✅ via `run`; ➖ not stored in state (crun doesn't either) |
| OCI 1.3 `freebsd.jail` | n/a | ❌ (uses `org.freebsd.*` annotations) | ✅ `freebsd-jail`, `freebsd-interface`, `freebsd-vnet-interfaces` |
| `domainname` | ✅ | ❌ ignored | ✅ `domainname` |
| `freebsd.devices` | n/a | ❌ | ✅ `freebsd-devices` |
| `process.rlimits` | ✅ | ❌ ignored | ✅ `process-rlimits` |
| `process.noNewPrivileges` | ✅ | ❌ ignored | ✅ `no-new-privs` |
| `process.consoleSize` | ✅ | ❌ ignored | ✅ `console-size` |
| `process.scheduler` | ✅ | ❌ ignored | ✅ `process-scheduler` (FIFO/RR/IDLE/nice) |
| Linux-only process fields (caps, AppArmor, SELinux, `oomScoreAdj`, `ioPriority`) | ✅ | ⚠️ silently ignored | ✅ rejected unless empty (`linux-process-fields`) |
| Resource limits (`linux.resources` → rctl) | cgroups | ❌ | ✅ `resource-limits` |
| `update` | ✅ (cgroups) | ❌ | ✅ `update` (on `resource-limits`) |
| `events` / stats | runc only | ❌ | ✅ `events` (on `resource-limits`) |
| `pause` / `resume` | ✅ (freezer) | ❌ | ✅ `pause-resume` (on `kill-all`): SIGSTOP sweep until every process has stopped |
| `checkpoint` / `restore` | ✅ (CRIU) | ❌ | ➖ out of scope (no CRIU) |
| seccomp, user ns, idmapped mounts, cgroups, mount propagation, rootless | ✅ | ❌ | ➖ out of scope (no FreeBSD equivalent) |

✅ done · ⚠️ buggy · ❌ missing · ⏳ to do · ➖ won't do

---

## Progress (updated 2026-10-26)

Every item is on its own branch in `grepjava/ocijail`. Each branch is
tested on FreeBSD 15.1: the new test fails without the change and
passes with it, and the full `//test:user` and `//test:root` suites
pass. Branches start from `upstream/main` unless the table says
otherwise. PRs #26–#34 and #36–#38 are open upstream; #36–#38 are the three bugs
found during this work. Other new PRs are on hold until dfr replies to
them and to dfr/ocijail#35.

| Phase | Item | Branch | State |
|---|---|---|---|
| 0 | `kill --all` | `kill-all` | PR #26 |
| 0 | `list --quiet` | `list-quiet` | PR #27 |
| 0 | read-only root unmount check | `readonly-root-unmount` | PR #28 |
| 0 | numeric `ociVersion` check | `oci-version-compare` | PR #29 |
| 0 | `ociVersion` reported by `state`/`features` | `oci-version-report` | PR #30 |
| 0 | log levels | `log-levels` | PR #31 |
| 0 | stopped detection with pid reuse | `stopped-pid-reuse` | PR #32 |
| 0 | `kill --pid` | `kill-pid` | PR #33 |
| 0 | log timestamp fraction | `log-timestamp` | PR #34 |
| 1 | `ps` | `ps` | pushed |
| 1 | `exec` command as arguments, unknown options | `exec-argv` | pushed |
| 1 | `exec --cwd/--env/--user` | `exec-flags` (on `exec-argv`) | pushed |
| 1 | `state`: `created`, `owner`, `rootfs` | `state-fields` | pushed |
| 1 | global `--debug`, `--systemd-cgroup`, `--cgroup-manager`, `--rootless` | `global-flags` | pushed |
| 1 | `spec` | `spec` | pushed |
| 1 | `run` (`--detach`, `--keep`, signal forwarding, 128+signal) | `run` | pushed |
| 2 | hook failure semantics and `timeout` | `hook-errors` | pushed |
| 2 | hook `args` are argv (bug found on the way) | `hook-argv` | PR #36 |
| 2 | `prestart` runs in `create` | `prestart-in-create` | pushed |
| 2 | `poststart` after exec; `start` reports exec failure | `poststart-after-exec` | pushed |
| 2 | stale state: verify jail by name and jid; `kill`/`exec` check status | `stale-state` | PR #37 |
| 2 | `creating` status; clean up after a failed `create` | `failed-create-cleanup` (standalone); `creating-status` is the same change stacked on `stale-state` | PR #38 |
| 2 | garbage jail error messages (bug found on the way) | `jail-errmsg` | pushed |
| 3 | `freebsd.jail` (all fields except `interface`, `vnetInterfaces`) | `freebsd-jail` | pushed |
| 3 | `freebsd.jail.interface` (aliases added/removed, pre-existing addresses left alone) | `freebsd-interface` (on `freebsd-jail`) | pushed |
| 3 | `freebsd.jail.vnetInterfaces` | `freebsd-vnet-interfaces` (on `freebsd-interface`) | pushed |
| 3 | `freebsd.devices` (devfs `unhide` rules on the container's `/dev`) | `freebsd-devices` | pushed |
| 3 | `domainname` → `host.domainname` | `domainname` | pushed |
| 3 | `freebsd.jail` section and `ociVersion` 1.3.0 in `spec` output | `spec-freebsd` (on `spec`) | pushed |
| 1 | `exec --no-new-privs` | `exec-no-new-privs` (on `exec-flags`, with `no-new-privs` cherry-picked) | pushed |
| 5 | `pause`/`resume`; `paused` handled by `kill`, `exec`, `delete --force` | `pause-resume` (on `kill-all`) | pushed |
| 5 | `linux.resources` → rctl rules; removed on `delete` | `resource-limits` | pushed |
| 5 | `update` (crun's flags and `-r`) | `update` (on `resource-limits`) | pushed |
| 5 | `events` / `events --stats` (runc's stats shape) | `events` (on `resource-limits`) | pushed |
| 4 | `rlimits` | `process-rlimits` | pushed |
| 4 | `noNewPrivileges` | `no-new-privs` | pushed |
| 4 | `consoleSize` | `console-size` | pushed |
| 4 | `scheduler` | `process-scheduler` | pushed |
| 4 | reject Linux-only process fields | `linux-process-fields` | pushed |
| 6 | jail parameter type checks abort with a parent jail (bug, podman pods) | `children-max` | pushed |
| 6 | detached `exec` of a missing command: error in `--log`, pid file only on success (bug) | `exec-error-log` | pushed |

All of the above are merged on `upstream/main` in the `next` branch,
which passes all 59 root tests and the user suites.

**Remaining:**

- Phase 0 infrastructure (CI, crun differential harness,
  runtime-tools). CI is deferred for now.
- Phase 6: buildah's system tests (podman's are done, see Phase 6).

**Decided against, with reasons:**

- Recording the exit status in the state: crun doesn't, and `run` gets
  it from `waitpid`.
- A flag to keep the old `prestart` timing: neither podman nor buildah
  uses `prestart` on FreeBSD.
- crun's `Error:` stderr prefix: podman reads errors from the JSON log.
- Rejecting a `linux` section outright: generic bundle tools emit one.
  Only the Linux-only `process` fields are rejected.
- A `freebsd` object in `features`: the spec's features schema doesn't
  define one.

**Found along the way:**

- Hook `args` had the path prepended, so hooks written to the spec got
  their own name as their first argument.
- A failing `createRuntime` hook left `create`'s child blocked forever.
- After a reboot with the state kept, `delete` could remove whichever
  jail had reused the jid.
- If `create` failed after mounting (for example because `jail_set`
  rejected a parameter), the mounts leaked inside the state directory,
  and a later `delete` ran `remove_all` into them.
- `jail_set`/`jail_get` error messages included uninitialised stack
  bytes.
- podman 5.8.9 and buildah on FreeBSD emit no `freebsd` section, no
  capabilities, and `RLIMIT_NOFILE`/`RLIMIT_NPROC` of 1048576 (podman)
  or 1024 (buildah). Jail settings still come from `org.freebsd.jail.*`
  annotations.
- `poststart`-after-exec needs the reader to open the fifo before
  signalling the container, or an error written by a dying child is
  lost.
- On FreeBSD, SIGCHLD under `SIG_DFL` is discarded even while blocked,
  so `run` installs a handler before calling `sigwait`.
- A jail can't use the realtime or idle scheduling classes, so
  `scheduler` is applied before `jail_attach`.
- `jail::get`/`jail::set` passed an `int32_t` jid and numeric
  `children.*`/`allow.*` values that `config::set`'s assertions
  rejected, so any build with assertions aborted on
  `org.freebsd.parentJail`. Release packages are built without them.
- A detached `exec` wrote its pid file before checking the command, so
  conmon took a failed exec as started: podman reported exit status 1
  instead of 127 (125 for `exec -d`) and kept the exec session.

**Integration notes** (for the `next` branch or merge order):

- `hook-errors`' test helper uses the old hook argv convention; prepend
  `"sh"` when it is combined with `hook-argv`.
- `kill-all`/`kill-pid`, `stale-state`/`stopped-pid-reuse`, and the
  branches that edit the `create.cpp` parent path (`hook-errors`,
  `prestart-in-create`, `creating-status`, `run`) conflict textually.

---

## Phase 0: Correctness and test infrastructure

**Status:** all 7 bugs are fixed (PRs #26–#32, plus `kill --pid` in #33 and the log timestamp in #34). The infrastructure isn't done: CI was skipped by request, and there's no crun harness or runtime-tools run yet.

*Goal: fix confirmed bugs and get automated FreeBSD testing in place before
adding features.*

**Bugs found while reading the code:**

1. `kill --all` is accepted but never used. `all_` is also uninitialised
   ([kill.h:20](../ocijail/kill.h#L20)). Implement it by signalling every
   process whose `ki_jid` matches the container jail (`sysctl
   kern.proc.all` / `kvm_getprocs`). This also serves `ps` and `pause` later.
2. `list --quiet` is declared with `add_option` instead of `add_flag`, and
   `quiet_` is never read ([list.cpp:26](../ocijail/list.cpp#L26)).
3. Read-only root unmount checks `unmount(...) > 0`. `unmount(2)` returns
   −1 on failure, so errors are silently swallowed
   ([create.cpp:516](../ocijail/create.cpp#L516),
   [delete.cpp:88](../ocijail/delete.cpp#L88)).
4. The OCI minor-version check compares strings (`ver.minor > "3"`), so
   `"10" > "3"` is false and 1.10 would be accepted
   ([create.cpp:127](../ocijail/create.cpp#L127)). Compare integers.
5. `state` always reports `ociVersion: "1.0.2"`, and `features` advertises
   `ociVersionMax: "1.2.0"` while `create` accepts 1.3.x. Make all three
   agree.
6. JSON log entries are always tagged `"level": "error"` regardless of the
   actual level. The levels also lack `error`, which crun accepts.
7. `stopped` is detected with `kill(pid, 0)`, which is unreliable once the
   pid is reused. Additionally confirm that the pid's `ki_jid` equals the
   container's jid.

**Infrastructure:**

- CI on a FreeBSD 14.x and 15.x VM (Cirrus CI, or GitHub Actions with
  `vmactions/freebsd-vm`), running `bazel test //test/...`.
- A **crun differential harness**: a table of `(bundle config, CLI
  invocation) → expected state/exit code/stderr shape` captured from crun on
  Linux and replayed against ocijail. Every later phase adds rows to it.
- Run the Linux-agnostic parts of
  [opencontainers/runtime-tools](https://github.com/opencontainers/runtime-tools)
  validation (lifecycle, state, hooks) and record the expected failures.

**Exit criteria:** all 7 bugs fixed with regression tests, CI green, harness
covering the existing commands.

---

## Phase 1: CLI surface parity

**Status:** done. `--process-label` and `--cap` weren't added. In `run`, a foreground container uses the caller's terminal rather than a pty that ocijail proxies.

*Goal: every crun subcommand podman calls exists and accepts crun's flags.*

- **`run`**: `create` + `start` + wait. Support `--detach`, `--bundle`,
  `--console-socket`, `--pid-file`, `--preserve-fds`, and propagate the
  container's exit code.
- **`ps`**: list the processes in the jail, with `--format table|json`
  (json is an array of pids, as in crun).
- **`exec` flags**: `--cwd`, `-e/--env`, `-u/--user uid[:gid]`,
  `--no-new-privs`, `--process-label` (accepted and ignored with a warning),
  and `--cap` (rejected unless empty). Allow building the process from argv
  when `--process` is absent, as crun does.
- **`spec`**: emit a default FreeBSD bundle `config.json` (devfs with ruleset
  4, a `freebsd.jail` section). Support `--bundle`.
- **`state`**: add `created` (RFC 3339), `owner`, and `rootfs`, matching
  crun's output.
- **`delete --force`**: verify that it matches crun for every state
  (`creating`, `paused`).
- **Global flags**: accept `--debug`, `--systemd-cgroup` (ignored with a
  warning), and `--rootless` (error unless `false`). Exit codes and the
  `Error:` stderr prefix should match crun, because podman scrapes them.

**Exit criteria:** podman's runtime calls all succeed, and the harness has
rows for every subcommand.

---

## Phase 2: Lifecycle semantics

**Status:** done. The exit status isn't recorded in the state (crun doesn't record it either), and there's no flag for the old `prestart` timing.

*Goal: lifecycle ordering and state transitions match the spec and crun.*

- **`prestart` / `createRuntime` timing.** The spec (and crun) runs
  `prestart` during `create`. ocijail runs it in `start`
  ([start.cpp](../ocijail/start.cpp)). Move it, and keep the legacy
  behaviour behind a flag in case podman depends on it.
- **`poststart` after exec.** `start` currently fires `poststart` as soon as
  it writes the fifo. Add a close-on-exec pipe from the container init so
  that `start` blocks until `execve` succeeds, returns an error if it fails,
  and only then runs `poststart`.
- **Exit status.** Record the init process's exit code in the state (via a
  reaper or `pdfork`/`procdesc`) so that `state` and `run` can report it.
- **`creating` status.** Write it before the jail is built, so a crashed
  `create` leaves a state that `delete` can clean up.
- **Hook timeouts.** Enforce `timeout` and treat failures as the spec
  requires: a failure in `prestart`, `createRuntime`, `createContainer` or
  `startContainer` aborts the container, while a failure in `poststart` or
  `poststop` only logs a warning.
- **Stale state recovery.** Handle a state directory whose jail is gone (for
  example after a host reboot) or a jail whose state is gone.

**Exit criteria:** the runtime-tools lifecycle and hooks validations pass,
and a hook-ordering test asserts the exact sequence.

---

## Phase 3: OCI 1.3 FreeBSD config section

**Status:** done (`freebsd-jail`, `freebsd-interface`, `freebsd-vnet-interfaces`, `freebsd-devices`, `domainname`, `spec-freebsd`). There's no `freebsd` object in `features`, because the spec's features schema doesn't define one.

*Goal: consume the standardised
[`freebsd`](https://github.com/opencontainers/runtime-spec/blob/main/config-freebsd.md)
section natively. This is the FreeBSD counterpart of crun reading `linux`.*

- Parse `freebsd.jail`: `parent`, `host`, `ip4`, `ip4Addr`, `ip6`,
  `ip6Addr`, `vnet`, `sysvmsg`, `sysvsem`, `sysvshm`, `enforceStatfs`, and
  `allow.*` (including `allow.mount[]` → `allow.mount.<fs>`).
- **Precedence:** the `freebsd` section wins over `org.freebsd.jail.*`
  annotations. Keep the annotations as a deprecated fallback for older
  podman versions.
- **Defaults:** the spec defaults `enforceStatfs` to 2 and `rawSockets` to
  false, whereas ocijail hardcodes 1 and true. Apply the spec defaults only
  when the `freebsd` section is present, so existing podman setups don't
  break.
- **`freebsd.jail.interface`**: add `ip4Addr`/`ip6Addr` aliases to the
  interface on create and remove them on delete. Record them in the state
  for cleanup.
- **`freebsd.jail.vnetInterfaces`**: `ifconfig <if> vnet <jid>` on create,
  and return the interface on delete.
- **`freebsd.devices`**: expose devices through per-container devfs rules
  (`DEVFSIO_RADD` or `devfs rule`) on the container's `/dev` mount, applying
  `mode`.
- **`features`**: add a `freebsd` object listing the supported jail fields
  and `allow` keys, and populate `annotations` and
  `potentiallyUnsafeConfigAnnotations`.

**Exit criteria:** a 1.3 bundle with only the `freebsd` section (no
annotations) behaves the same as today's annotation-driven bundles, with
tests for each field.

---

## Phase 4: Process attributes

**Status:** done. `scheduler` supports FIFO/RR (POSIX priorities 0–31), IDLE (idprio 31) and nice. `ioPriority` and the Linux-only fields are rejected unless they ask for nothing.

*Goal: honour the portable `process` fields that crun applies.*

| Field | FreeBSD implementation |
|---|---|
| `rlimits` | `setrlimit()` before exec. Map `RLIMIT_*` names and reject unknown ones (e.g. `RLIMIT_RTTIME`). |
| `noNewPrivileges` | `procctl(P_PID, 0, PROC_NO_NEW_PRIVS_CTL, PROC_NO_NEW_PRIVS_ENABLE)` (FreeBSD 14+). Error on older releases. |
| `consoleSize` | `ioctl(TIOCSWINSZ)` on the pty before handoff. |
| `scheduler` | Partial: `rtprio`/`idprio` for the RR, FIFO and IDLE classes. Reject the rest. |
| `ioPriority` | No equivalent: reject a non-default value. |
| `capabilities`, `apparmorProfile`, `selinuxLabel`, `oomScoreAdj` | Linux-only. Ignore if empty or default, otherwise error, and leave them out of `features`. |

**Exit criteria:** each field has a run test inside a jail (for example
`ulimit -n` reflects the configured `rlimits`).

---

## Phase 5: Resource control (the cgroups counterpart)

**Status:** done. The test host now has `kern.racct.enable=1` (set 2026-10-26).

- **podman doesn't send limits on FreeBSD.** podman 5.8.9 drops `--memory`, `--memory-swap`, `--cpus` and `--device-read-bps` without sending them to the runtime; there is no `linux` section in its config. Only `io.podman.annotations.pids-limit` survives, as an annotation. ocijail enforces `linux.resources` as crun does, for any tool that sends it, but podman's limits won't take effect until podman passes them.
- **Mapping.** `memory.limit` → `memoryuse:sigkill`, because `memoryuse:deny` doesn't stop allocation (memory is only counted once it is resident) and `vmemoryuse:deny` would break runtimes that reserve large address spaces. `memory.swap` → `swapuse:deny` on the difference. `pids.limit` → `maxproc:deny`. `cpu.quota`/`period` → `pcpu:deny` (percent of one CPU). Block I/O throttles → jail-wide `readbps`/`writebps`/`readiops`/`writeiops` throttles at the lowest rate given.
- **rctl rules are kept by jail name and outlive the jail,** so `delete` removes them.
- `pause`/`resume` (`pause-resume`) use a SIGSTOP sweep.

*Goal: give `update`, `events --stats`, `pause` and `resume` real
implementations backed by rctl/racct. This requires the
`kern.racct.enable=1` boot tunable, which the runtime detects and reports
in `features`.*

- Map the portable subset of `linux.resources` (which podman emits) to
  `rctl` rules on `jail:<name>`:

  | OCI | rctl |
  |---|---|
  | `memory.limit` | `memoryuse:deny` |
  | `memory.swap` | `swapuse:deny` |
  | `pids.limit` | `maxproc:deny` |
  | `cpu.quota` / `cpu.period` | `pcpu:deny` (percentage) |
  | `blockIO.throttle*` | `readbps`/`writebps`/`readiops`/`writeiops:throttle` |
  | (none; FreeBSD only) | `openfiles`, `vmemoryuse` |

- **`update`**: re-apply the rules (`--memory`, `--pids-limit`,
  `--cpu-quota`, `-r resources.json`).
- **`events --stats`**: report rctl usage (`rctl -u jail:<name>`) in runc's
  stats JSON shape.
- **`pause` / `resume`**: SIGSTOP/SIGCONT every process in the jail, using
  the Phase 0 process enumeration, and track `paused` in the state.
  **Caveat:** this isn't atomic like the cgroup freezer. A process can fork
  during the sweep, so loop until no new pids appear.
- Remove the rules on `delete`.

**Exit criteria:** a memory-limit test sees an allocation failure, a
`pids.limit` test sees fork failure, and pause/resume survives a fork bomb.

---

## Phase 6: Ecosystem validation

**Status:** podman done (2026-10-26); buildah not started.

podman 5.8.9's system tests (`test/system`, 17 files: basic, info, run,
sig-proxy, logs, ps, start, stop, rm, exec, pause, top, kill, volumes,
pod, update, interactive) were run with the released ocijail 0.6.0
package and with `next`, on podman storage of their own. There is no
crun on FreeBSD, so 0.6.0 is the baseline.

Running them on FreeBSD needs GNU coreutils and sed first in `PATH`
(FreeBSD's `timeout -v` reports every exit; `expr` has no `\+`) and a
FreeBSD stand-in for podman's test image: `freebsd-notoolchain:15.1`
with podman's `/home/podman` helpers, `sleep inf` and `top` that keep
running, and `/bin/true`, `/bin/false`, `/bin/printenv`.

| | passed (of 247) |
|---|---|
| ocijail 0.6.0 | 158 |
| `next` | 162 |

Differences:

- `next` passes and 0.6.0 fails: maximum ulimit (`process.rlimits`),
  `pause`/`unpause` and `unpause --all`, `exec - basic test` and
  `exec - does not leak session IDs on invalid command` (the
  `exec-error-log` fix).
- 0.6.0 passes and `next` fails: `logs --since` and `--until` with
  k8s-file. podman always stops a FreeBSD container with `kill --all`
  (there's no pid namespace); 0.6.0 ignores `--all`, `next` signals
  every process like crun, so the test's `sleep` child is killed too and
  its shell prints `Terminated`. crun does the same for a container
  sharing the host's pid namespace.

The failures both share are FreeBSD or environment differences, not
runtime ones:

- No pid namespace: no pid 1 (`top` expects it; `sleep` as pid 1
  doesn't ignore SIGTERM, so `stop`'s SIGKILL warning and the
  `stopping` state never appear).
- podman on FreeBSD: no journald or cgroups, `update` panics with a nil
  pointer before calling the runtime, `pod top` and `ps` use Linux `ps`
  options, `volume create -o type=bind` doesn't check the device.
- Linux userland in the tests: `/proc/self/status`, GNU `stat -c`,
  signal 64, busybox `sh` exit status after a trapped signal, lower-case
  `permission denied`, `ip`, images pulled from quay.io.
- Mounts: ocijail mounts with `MNT_IGNORE` so that container mounts
  don't fill the host's `mount` and `df`; inside the jail, plain `mount`
  then lists only `/` (`mount -v` lists them all). Linux gets both from
  mount namespaces; a jail shares the host's mount table, so this is a
  trade-off, not a bug.

Two ocijail bugs found (see Progress): `children-max` and
`exec-error-log`.

- Run buildah's system tests the same way.
- Produce a single parity report (this table, auto-generated from the
  harness) on each release.
- Keep `sysutils/ocijail` in the FreeBSD ports tree in step with the
  feature set.

---

## Out of scope

These have no FreeBSD kernel equivalent. ocijail should **reject** non-empty
requests for them rather than silently ignore them:

- `checkpoint` / `restore` (CRIU)
- seccomp (Capsicum is opt-in by the application, not imposed by the
  runtime)
- Linux capabilities. The jail `allow.*` params and `suser` are the
  substitute, through `freebsd.jail.allow`.
- User namespaces, idmapped mounts and rootless mode (jails require root)
- cgroups, `intelRdt`, `memoryPolicy`, time namespaces, `personality`
- Mount propagation (`rshared`, `rslave`). nullfs isn't recursive, so
  `rbind` stays a documented approximation.

## Suggested order and sizing

| Phase | Size | Depends on |
|---|---|---|
| 0: Correctness and CI | S | n/a |
| 1: CLI surface | M | 0 |
| 2: Lifecycle semantics | M | 0 |
| 3: OCI 1.3 `freebsd` section | M | 0 |
| 4: Process attributes | S | 0 |
| 5: Resource control | L | 0 (process enumeration), 1 |
| 6: Ecosystem validation | ongoing | all |

Phases 1–4 are independent of each other and can run in parallel after
Phase 0.
