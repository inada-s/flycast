# Rollback netcode test suite

Run this after touching the GGPO library (`core/deps/ggpo`), `core/network/ggpo.cpp`, or the rollback
backend (`core/gdxsv/gdxsv_backend_rollback.*`). Each case runs four flycast instances as a local
rollback match (`gdxsv:rbk_test=N/4`) with random input, and checks the logs and state hash logs.

## Running

```
python tools/rbk_test/run_suite.py --rom D:\rom\gdx-disc2\gdx-disc2.gdi --old-release gdxsv-1.9.2 --older-release gdxsv-1.8.13
python tools/rbk_test/run_suite.py --rom ... --cases C1,C2     # selected cases
python tools/rbk_test/run_suite.py --rom ... --long            # include A2 (about 20 minutes)
python tools/rbk_test/run_suite.py --list
python tools/rbk_test/run_suite.py --rom ... --jobs 3                  # cases in parallel
```

- `--exe` defaults to `cmake-build-relwithdebinfo/flycast.exe`.
- `--jobs N` runs N cases at once, each on its own ports (`TEST_PORT_BASE`, local relays too). A3 and A4 run
  release builds, which always use the default ports, so those two take turns. Each case runs four emulators;
  keep N well under the CPU count, or timing-sensitive cases (stalls, B1) may fail from load alone.
- `--old-release` / `--older-release` download a release with the GitHub CLI (`gh`). Use the latest
  release and one before it. Pass `--old-exe` / `--older-exe` to use local builds instead. Without
  them, A3/A4 are skipped.
- The pre-battle savestate (`gdx-disc2_99.state`) is taken from `work/state` and downloaded if missing.
- Output goes to `tools/rbk_test/out/<case>/p1..p4/` (`flycast.log`, `data/hashlog.txt`). The copied
  executables are deleted after each match. `out/summary.json` holds the results.
- The whole default set takes about 30 minutes. Windowed cases (B2, B3, C1, C3) open four emulator
  windows, and audio is muted. C1 is Windows only.
- The exit code is 1 if any case fails. `INCONCLUSIVE` means the situation a case looks for did not
  happen in that run (for example, no rollback crossed a timesync skip). Rerun it.

Every match is checked for the following:
- Every peer logs `RollbackNet local test finished`.
- All peers have the same round winners (`WIN_TEAM`), and their last frames in the hash log differ by at most 2. Teardown timing can leave one peer a frame ahead.
- No log contains `GGPO error`, `Exception`, `[GPF]`, `Assertion` or `Flycast has stopped`.

A failed ROM load still exits with code 0, so the suite checks the logs, not the exit code.

## Cases

| ID | Case | Setup | Pass when |
|---|---|---|---|
| A1 | Basic match | headless, send delay 16/16/16/100 ms | common checks |
| A2 | 20 rounds (`--long`) | `MAXREBATTLE=20` | common checks; used MS and StartMsg/LoadEndMsg join frames identical on all peers |
| A3 | Mixed with the previous release | old ×2 + new ×2, run with peer 0 old and with peer 0 new | common checks |
| A4 | Mixed with an older release | same as A3 | common checks |
| B1 | Timesync skip replayed in a rollback | seeds 11..66, `GGPO_TEST_LOG=1` | every rollback whose re-simulated range contains a skip after the seek frame replays it |
| B2 | Threaded rendering | windowed, delay 50/16/80/100, 3 rounds | common checks, no stall, rollbacks happened |
| B3 | Threaded rendering off | windowed, `rend.ThreadedRendering=no` | common checks (the harness gets no rollbacks in this mode, old builds too) |
| C1 | Peer drops mid-battle | windowed, stat OSD on; peer 4 killed 10 s into the battle, `Process.Responding` of the others sampled for 40 s | no sample not responding; peers 1-3 close the session |
| C2 | GGPO session start times out | peer 4 killed 5 s into the ping test; 3 runs | peers 1-3 log `StartNetwork timeout` and close, no crash |
| C3 | Re-battle cancel ends the match | `RAND_MASK=06F6`, `MAXREBATTLE=3`, `GGPO_TEST_LOG=1`; headless and windowed | all peers enter scene 4/3 on the same frame and are in CloseWait at 4/4; common checks |
| D1 | Malformed packets | ~45 s of `fuzz.py` during the battle (oversized AppData/Input, truncated, unknown types, bogus relays) | common checks |
| D2 | Relay in use | `TEST_RELAY=single` | p1 picks the relay (`Relay:1`), p2 forwards, common checks |
| D3 | Relay loop | `TEST_RELAY=loop` | peers 1 and 2 cut the loop (a few `relay drop-return-to-sender` lines, not a flood), no crash; session start times out as expected |
| D4 | Relay server | a local `gdxsv relay` (`--relay-exe`, default `../gdxsv/bin/gdxsv.exe`), `TEST_RELAY=server` | p1 picks the server (`Relay:2`), every peer sends through it (the others after p1's packets reach them through it), the relay bound all four peers, common checks. Skipped without the binary |
| D5 | Two relay servers | two local relays, `TEST_RELAY=server2` | p1 starts on server 1 for peer 1 and moves to server 0, which peer 1 picked; peer 1 stays on server 0; peers 2 and 3 answer peer 0 through server 1; common checks |
| D6 | Relay server across IP families | a dual-stack local relay, `TEST_RELAY=server`, `TEST_RELAY_IPV6=::1` | peers 1-2 get only a reachable IPv6 address for the relay and pick it, peers 3-4 use IPv4 (the relay binds 2 and 2), peer 0 relays to everyone, common checks. The relay listens on all addresses, which may raise a firewall prompt |
| D7 | Lower RTT wins between relays | a local relay, `TEST_RELAY=fair` | p1 reaches p4 through the relay server (~35 ms) rather than through p2 (50 ms), though the server is less than 16 ms faster; common checks |

## Test-only switches (environment variables)

These do nothing unless set.

| Variable | Effect | Where |
|---|---|---|
| `GGPO_NETWORK_DELAY` | Send latency in ms (GGPO, existing) | any session |
| `VITAL`, `MAXREBATTLE` | Team vitals and number of re-battles (existing) | local test |
| `RAND_MASK` | Hex kcode mask for the random input. The default leaves out down/right, so the re-battle menu is never cancelled; `06F6` reaches it | local test |
| `TEST_RELAY` | `single`: peer 0 reaches peer 3 through peer 1. `loop`: peer 1 also reaches peer 3 through peer 0. `server`: peer 0 reaches every peer through the relay server. `server2`: two relay servers, peer 0 picks the second and peer 1 the first. `fair`: peer 0 reaches peer 3 through peer 1 (50 ms) or the relay server, whichever is faster | local test |
| `TEST_RELAY_SERVER`, `TEST_RELAY_TOKEN`, `TEST_RELAY_IPV6` | Adds relay servers (`ip:port`, comma separated; hex token, default `1234`; for peers 0-1, every relay gets this IPv6 address and an unreachable IPv4 one) to the local match, for a relay started with `-relay_test_session=12345:<token>` | local test |
| `RBK_SAVE_REPLAY` | Saves the match to `data/replays/0123456.pb` when it ends (never uploaded) | local test |
| `TEST_PORT_BASE` | First of the peers' ports (default 20010, peer N uses base + N), so local tests can run at once | local test |
| `GGPO_TEST_LOG` | Logs `RBKTEST` lines: timesync skip record/replay, rollback loads, scene changes, relay forwards and loop drops, relay server use | any session |

## Not covered here

Check these by hand against a real server:
- Cancelling the re-battle menu with real input.
- Start failure paths that need the server or real peers, such as `delay_too_large` or `unreachable`.
- Relay through real NATs.
- Spectators.
