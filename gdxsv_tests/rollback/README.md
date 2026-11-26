# Rollback netcode test suite

Run this after touching the GGPO library (`core/deps/ggpo`), `core/network/ggpo.cpp`, or the rollback
backend (`core/gdxsv/gdxsv_backend_rollback.*`). Each case runs four flycast instances as a local
rollback match (`gdxsv:rbk_test=N/4`) with random input, and checks the logs and state hash logs.

## Running

```
python gdxsv_tests/rollback/run_suite.py --rom D:\rom\gdx-disc2\gdx-disc2.gdi --old-release gdxsv-1.9.2 --older-release gdxsv-1.8.13
python gdxsv_tests/rollback/run_suite.py --rom ... --cases C1,C2     # selected cases
python gdxsv_tests/rollback/run_suite.py --rom ... --long            # include A2 (about 20 minutes)
python gdxsv_tests/rollback/run_suite.py --list
python gdxsv_tests/rollback/run_suite.py --rom ... --jobs 3                  # cases in parallel
```

- `--exe` defaults to `cmake-build-relwithdebinfo/flycast.exe`.
- `--jobs N` runs N cases at once, each on its own ports (`TEST_PORT_BASE`, local relays too). A3, A4 and their controls run
  release builds, which always use the default ports, so they take turns. Each case runs four emulators;
  keep N well under the CPU count, or timing-sensitive cases (stalls, B1) may fail from load alone.
- `--old-release` / `--older-release` download a release with the GitHub CLI (`gh`). Use the latest
  release and one before it. Pass `--old-exe` / `--older-exe` to use local builds instead. Without
  them, A3, A4 and their controls are skipped.
- The pre-battle savestate (`gdx-disc2_99.state`) is taken from `work/state` and downloaded if missing.
- Output goes to `gdxsv_tests/rollback/out/<case>/p1..p4/` (on Windows: `flycast.log`, `data/hashlog.txt`;
  the layout per OS is in `gdxsv_tests/lib/emu.py`). Copied executables (Windows) are deleted after each
  match. `out/summary.json` holds the results.
- The whole default set takes about 36 minutes. Windowed cases (B2, C1, C3) open four emulator
  windows, and audio is muted. C1 is Windows only.
- The exit code is 1 if any case fails. `INCONCLUSIVE` means the situation a case looks for did not
  happen in that run (for example, no rollback crossed a timesync skip). Rerun it.
- A control (`<ID>c`) runs its parent's checks on a broken setup and passes only when they fail on the
  expected sign: it shows the parent's checks can fail.

Every match is checked for the following:
- Every peer logs `RollbackNet local test finished`.
- All peers have the same round winners (`WIN_TEAM`), and their last frames in the hash log differ by at most 2. Teardown timing can leave one peer a frame ahead.
- No log contains `GGPO error`, `Exception`, `[GPF]`, `Assertion` or `Flycast has stopped`.

A failed ROM load still exits with code 0, so the suite checks the logs, not the exit code.

## Cases

| ID | Case | Setup | Pass when |
|---|---|---|---|
| A1 | Basic match | headless, send delay 16/16/16/100 ms | common checks |
| A1c | Control of A1 | as A1, peer 2 loads `desync.lua` (holds player 1 HP at 1 from battle step 600) | A1's checks fail on last frames or round winners |
| A2 | 20 rounds (`--long`) | `MAXREBATTLE=20` | common checks; used MS and StartMsg/LoadEndMsg join frames identical on all peers |
| A3 | Mixed with the previous release | old ×2 + new ×2, run with peer 0 old and with peer 0 new | common checks |
| A4 | Mixed with an older release | same as A3 | common checks |
| B1 | Timesync skip replayed in a rollback | one match, `GGPO_TEST_LOG=1`, `TEST_GGPO_DELAY=1` (deeper rollbacks), `TEST_FAKE_TIMESYNC=20` (peer 0 skips every 20 frames) | at least one rollback re-simulates a skip after its seek frame; every such rollback replays it |
| B2 | Threaded rendering | windowed, delay 50/16/80/100, 3 rounds | common checks, no stall, rollbacks happened |
| C1 | Peer drops mid-battle | windowed, stat OSD on; peer 4 killed 10 s into the battle, `Process.Responding` of the others sampled for 40 s | no sample not responding; peers 1-3 close the session |
| C2 | GGPO session start times out | peer 4 killed 5 s into the ping test; 3 runs | peers 1-3 log `StartNetwork timeout` and close, no crash |
| C3 | Re-battle cancel ends the match | `RAND_MASK=06F6`, `MAXREBATTLE=3`, `GGPO_TEST_LOG=1`; headless and windowed | all peers enter scene 4/3 on the same frame and are in CloseWait at 4/4; common checks |
| D1 | Malformed packets | ~45 s of `fuzz.py` during the battle (oversized AppData/Input, truncated, unknown types, bogus relays), `GGPO_TEST_LOG=1` | a peer logs `RBKTEST malformed drop` (relayed malformed AppData); common checks |
| D2 | Relay in use | `TEST_RELAY=single` | p1 picks the relay (`Relay:1`), p2 forwards, common checks |
| D3 | Relay loop | `TEST_RELAY=loop` | peers 1 and 2 cut the loop (a few `relay drop-return-to-sender` lines, not a flood), no crash; session start times out as expected |
| D4 | Relay server | a local `gdxsv relay` (`--relay-exe`, default `../gdxsv/bin/gdxsv.exe`), `TEST_RELAY=server` | p1 picks the server (`Relay:2`), every peer sends through it (the others after p1's packets reach them through it), the relay bound all four peers, common checks. Skipped without the binary |
| D5 | Two relay servers | two local relays, `TEST_RELAY=server2` | p1 starts on server 1 for peer 1 and moves to server 0, which peer 1 picked; peer 1 stays on server 0; peers 2 and 3 answer peer 0 through server 1; common checks |
| D6 | Relay server across IP families | a dual-stack local relay, `TEST_RELAY=server`, `TEST_RELAY_IPV6=::1` | peers 1-2 get only a reachable IPv6 address for the relay and pick it, peers 3-4 use IPv4 (the relay binds 2 and 2), peer 0 relays to everyone, common checks. The relay listens on all addresses, which may raise a firewall prompt |
| D7 | Lower RTT wins between relays | a local relay, `TEST_RELAY=fair` | p1 reaches p4 through the relay server (~35 ms) rather than through p2 (50 ms), though the server is less than 16 ms faster; common checks |
| D8 | Symmetric NAT peer | a local relay, `TEST_RELAY=nat` | p2 reaches only p4 in the ping test, and the way through p4 is slow; p1 and p3 reach p2, and p2 reaches them, through the relay server (`Relay:2`), using the relay RTTs p4 passed on; common checks. With `--old-exe`/`--old-release`, again with p2 on that build: p1 and p3 still pick the relay server and p2 answers through it |
| D9 | Isolated peer | a local relay, `TEST_RELAY=isolated` | p2 reaches no peer directly; every path to and from p2 goes through the relay server (`Relay:2`), and each side got RTT rows from the other through it (`RTTS BY RELAY`); common checks. With `--old-release`, p1 runs it: p2 still reaches p1, p3, p4 through the relay server |
| E1 | Rollback budget update | one match from fresh directories | every peer uses `rollback_budget=1.00`, logs the update over at least 1800 frames as halfway to clamp(fit − 1, 0, 1.5) and saves it (`emu.cfg` `RollbackBudget`); common checks |

## Test-only switches (environment variables)

These do nothing unless set.

| Variable | Effect | Where |
|---|---|---|
| `GGPO_NETWORK_DELAY` | Send latency in ms (GGPO, existing). Each packet goes at 2/3 to 1 of it, drawn again on every send-queue pump, so most go near 2/3 | any session |
| `GGPO_NETWORK_JITTER` | Each packet waits `GGPO_NETWORK_DELAY` + 0..N ms, drawn once when queued (packets stay in order) | any session |
| `GGPO_NETWORK_LOSS` | Drops N percent of GGPO packets instead of sending them; logs `GGPO_NETWORK_LOSS drop n=.. of .. packets` every 20 drops | any session |
| `VITAL`, `MAXREBATTLE` | Team vitals and number of re-battles (existing) | local test |
| `RAND_MASK` | Hex kcode mask for the random input. The default leaves out down/right, so the re-battle menu is never cancelled; `06F6` reaches it | local test |
| `TEST_RELAY` | `single`: peer 0 reaches peer 3 through peer 1. `loop`: peer 1 also reaches peer 3 through peer 0. `server`: peer 0 reaches every peer through the relay server. `server2`: two relay servers, peer 0 picks the second and peer 1 the first. `fair`: peer 0 reaches peer 3 through peer 1 (50 ms) or the relay server, whichever is faster. `nat`: peer 1 hears only from peer 3 in the ping test, and the way through peer 3 is 100 ms. `isolated`: peer 1 hears from no peer directly | local test |
| `TEST_RELAY_SERVER`, `TEST_RELAY_TOKEN`, `TEST_RELAY_IPV6` | Adds relay servers (`ip:port`, comma separated; hex token, default `1234`; for peers 0-1, every relay gets this IPv6 address and an unreachable IPv4 one) to the local match, for a relay started with `-relay_test_session=12345:<token>` | local test |
| `RBK_SAVE_REPLAY` | Saves the match to `data/replays/0123456.pb` when it ends (never uploaded) | local test |
| `TEST_PORT_BASE` | First of the peers' ports (default 20010, peer N uses base + N), so local tests can run at once | local test |
| `GGPO_TEST_LOG` | Logs `RBKTEST` lines: timesync skip record/replay, rollback loads, scene changes, relay forwards and loop drops, malformed packet drops, relay server use | any session |
| `GGPO_TEST_NO_SKIP_REPLAY` | A rollback does not replay the timesync skips after its seek frame | local test |
| `TEST_GGPO_DELAY` | Fixed input delay in frames instead of one derived from the ping test, so a large `GGPO_NETWORK_DELAY` means deep prediction rather than more delay | local test |
| `TEST_FAKE_TIMESYNC` | Peer 0 takes a timesync skip every K frames (K = value), as a peer whose clock runs fast does. Peers on one machine rarely get them | local test |
| `GGPO_TEST_NO_REBATTLE_CANCEL` | The re-battle-cancel scene does not close the match | local test |
| `TEST_SPECTATOR_LBS` | Streams the peers' confirmed inputs to a local lbs (`ip:port`) started with `-spectator_test_session` | local test |
| `TEST_SPECTATOR_UPLINK` | Which peer lbs would ask to stream (peer number, default 1), or `all` as clients from before lbs picked one did | local test |
| `TEST_UPLINK_ROUND_DELAY` | Sends round starts to lbs N frames late, so spectators reach StartMsg before the round's start index | local test |

## Recorded battle over rollback

`replay_match.py` plays a recorded battle (a replay of a rollback battle, e.g. from production) as a local
rollback match, for looking at frame pacing with real input. Each peer runs with `gdxsv:rbk_replay=<replay>`:
it gives GGPO its player's recorded input, and the game reads the recorded inputs of all players by KeyMsg1
number, as replay playback does. So the battle is the recorded one whatever GGPO predicts, while rollbacks
and timesync run as in a real match. Users, rule and online patches come from the replay; random input is off.

```
python gdxsv_tests/rollback/replay_match.py --rom D:\rom\gdx-disc2\gdx-disc2.gdi --delays 30,50,60,80
python gdxsv_tests/rollback/replay_match.py --rom ... --replay <battle>.pb
```

Without `--replay` it plays the newest Disc 2 corpus replay (`--corpus <id>` for another one from
`gdxsv_tests/corpus/replays.json`). Only replays with inputs and round starts work: corpus 1727284586532 and newer.

It reports per peer each round's start RNG against the recording (`RBKREPLAY round N ... match|DESYNC`), the
round winners against the recording, and how many local inputs GGPO carried that differ from the recording
(`RBKREPLAY local inputs`; the input added for a frame is guessed by assuming one KeyMsg1 per frame).
`--window` opens the four windows; a full battle takes about as long as it did.

## Live spectator rig

`live_rig.py` runs a match through a local lbs to live spectators, the same path as production: the
peers' uplink (`TEST_SPECTATOR_LBS`), lbs assembling the live recording, and spectators joining with
`gdxsv:spectate`. It needs a gdxsv build with `-spectator_test_session` (`--gdxsv`, default
`../gdxsv/bin/gdxsv.exe`) and a Disc 2 replay of a local test for lbs to hand spectators the users and
rule (`--make-header` makes one).

```
python gdxsv_tests/rollback/live_rig.py --rom D:\rom\gdx-disc2\gdx-disc2.gdi --make-header --tag livehdr --rounds 1
TEST_FAKE_TIMESYNC=20 python gdxsv_tests/rollback/live_rig.py --rom D:\rom\gdx-disc2\gdx-disc2.gdi \
    --header gdxsv_tests/rollback/out/livehdr/p1/data/replays/0123456.pb --rounds 4 --delays 30,50,60,80 --input-delay 3
```

lbs asks the participant nearest it to stream (`spectator_uplink` in P2PMatching), and records only
the first peer whose upload arrives, which also covers older clients that all stream
(`TEST_SPECTATOR_UPLINK=all`). The rig checks that lbs's recording is exactly that peer's replay,
and that each spectator's battle state, logged once per sim step by `live_det.lua`, matches that
replay played offline. With the settings above, a round often ends one
input later on some peers than on others: the game sends a KeyMsg1 between rounds, and a peer taking
a timesync skip on it never records it. lbs used to merge all peers and got the next round's start
wrong then. Add `TEST_UPLINK_ROUND_DELAY=180` to make spectators wait at StartMsg for the round start
(`StartMsg held` in their log); `--spectators 1:0,3:120` adds one that joins mid-match.
`--spectator-extra "--config gdxsv:ReplayFourScreen=yes"` makes the spectator a four-screen host.

## Live autoplay

lbs never closes a `-spectator_test_session`, so moving on from one battle to the next needs
`fake_live_lbs.py`: it serves a replay over lbs's spectator UDP channel as a series of live battles
in real time, closes each at its end, and answers `/status` for the Live tab.

```
python gdxsv_tests/rollback/fake_live_lbs.py --replay gdxsv_tests/rollback/out/<tag>_header.pb.live.pb --battles 3 --max-frames 5300 --overlap 60
flycast --config gdxsv:server=127.0.0.1 --config gdxsv:LiveApiUrl=http://127.0.0.1:3390 \
    --config gdxsv:spectate=9000000 --config gdxsv:ReplayFourScreen=yes --config gdxsv:LiveAutoNext=yes <rom>
```

`--max-frames 5300` cuts each battle in round 3's MS selection, like a disconnect between rounds;
`--overlap` starts the next battle while one is still on, `--gap` after it ends instead.
Give several `--replay`s to serve them in turn: battles that differ catch state carried over from
the previous one. The replay
must come from a local test (an lbs recording from `live_rig.py` works), so the bootstrap savestate
and rule match.

## Not covered here

Check these by hand against a real server:
- Cancelling the re-battle menu with real input.
- Start failure paths that need the server or real peers, such as `delay_too_large` or `unreachable`.
- Relay through real NATs.
- Spectators beyond `live_rig.py`: joining through the lobby UI, the close report, real network loss.
