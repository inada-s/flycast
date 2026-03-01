#!/usr/bin/env python3
"""
Batch Replay Tool for GDXSV Battle Record Data Extraction

Reads battle records from SQLite, replays each battle using flycast in batch mode,
and updates the database with extracted round/win/lose/used_ms data.

Usage:
    python batch_replay.py --db gdxsv.db --flycast ./flycast.exe --gdi path/to/game.gdi [options]
"""

import argparse
import functools
import glob
import json
import logging
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

print = functools.partial(print, flush=True)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
logger = logging.getLogger(__name__)

# Directory for savestate files
STATE_DIR = "work/state"


def download_state():
    """Download replay savestates if not present."""
    os.makedirs(STATE_DIR, exist_ok=True)
    for name, url in [
        ("gdx-disc1_99.state", "https://storage.googleapis.com/gdxsv/misc/gdx-disc1_99.state"),
        ("gdx-disc2_99.state", "https://storage.googleapis.com/gdxsv/misc/gdx-disc2_99.state"),
    ]:
        path = os.path.join(STATE_DIR, name)
        if not os.path.isfile(path):
            logger.info(f"Downloading {name}...")
            urllib.request.urlretrieve(url, path)


def prepare_workdir(idx: int, flycast_path: str) -> str:
    """
    Prepare an isolated working directory for flycast instance.
    Each instance gets its own copy of the executable and state files.
    Returns the working directory path.
    """
    wdir = f"work/batch{idx}"
    data_dir = os.path.join(wdir, "data")
    os.makedirs(data_dir, exist_ok=True)

    flycast_name = Path(flycast_path).name
    dest_exe = os.path.join(wdir, flycast_name)
    if not os.path.isfile(dest_exe) or os.path.getmtime(flycast_path) > os.path.getmtime(dest_exe):
        shutil.copy(flycast_path, dest_exe)

    # Copy state files
    for file in glob.glob(os.path.join(STATE_DIR, "*.state")):
        dest = os.path.join(data_dir, os.path.basename(file))
        if not os.path.isfile(dest):
            shutil.copy(file, dest)

    return wdir


def download_replay(replay_url: str, dest_path: str) -> bool:
    """Download a replay .pb file from the given URL."""
    try:
        urllib.request.urlretrieve(replay_url, dest_path)
        return True
    except Exception as e:
        logger.error(f"Failed to download {replay_url}: {e}")
        return False


def run_flycast_batch(
    wdir: str,
    gdi_path: str,
    pb_path: str,
    pov: int = 1,
    timeout: int = 300,
) -> dict | None:
    """
    Run flycast in batch replay mode from the given working directory.
    Returns the parsed JSON dict, or None on failure.
    """
    flycast_name = None
    for f in os.listdir(wdir):
        if f.endswith(".exe") or (os.access(os.path.join(wdir, f), os.X_OK) and not os.path.isdir(os.path.join(wdir, f))):
            if "flycast" in f.lower():
                flycast_name = f
                break
    if flycast_name is None:
        flycast_name = "flycast.exe"

    # Use absolute path to exe (shell=True with bare name may fail on Windows)
    exe_path = os.path.join(wdir, flycast_name)

    # Remove previous result file
    result_file = os.path.join(wdir, "batch_result.json")
    if os.path.exists(result_file):
        os.remove(result_file)

    cmd = [
        exe_path,
        "-config", f"gdxsv:replay={pb_path},gdxsv:ReplayPOV={pov},gdxsv:batch_replay=1",
        gdi_path,
    ]

    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=wdir,
        )
    except subprocess.TimeoutExpired:
        logger.error(f"Flycast timed out after {timeout}s for {pb_path}")
        return None
    except Exception as e:
        logger.error(f"Flycast execution error: {e}")
        return None

    # Primary: read result from file (WIN32 apps don't have stdout connected)
    if os.path.exists(result_file):
        try:
            with open(result_file, "r") as f:
                return json.load(f)
        except (json.JSONDecodeError, IOError) as e:
            logger.error(f"Failed to read batch_result.json: {e}")

    # Fallback: search stdout for BATCH_RESULT line
    for line in proc.stdout.splitlines():
        if line.startswith("BATCH_RESULT:"):
            json_str = line[len("BATCH_RESULT:"):]
            try:
                return json.loads(json_str)
            except json.JSONDecodeError as e:
                logger.error(f"Failed to parse JSON: {e}\n  Line: {json_str}")
                return None

    # Fallback: check stderr
    for line in proc.stderr.splitlines():
        if line.startswith("BATCH_RESULT:"):
            json_str = line[len("BATCH_RESULT:"):]
            try:
                return json.loads(json_str)
            except json.JSONDecodeError as e:
                logger.error(f"Failed to parse JSON from stderr: {e}")
                return None

    logger.error(f"No BATCH_RESULT found for {pb_path}")
    if proc.returncode != 0:
        logger.error(f"Flycast exited with code {proc.returncode}")
    stderr_lines = proc.stderr.splitlines()[-5:]
    if stderr_lines:
        logger.error(f"Last stderr: {stderr_lines}")
    return None


def process_one_battle(
    battle_code: str,
    replay_url: str,
    wdir: str,
    gdi_path: str,
    pov: int,
    timeout: int,
    temp_dir: str,
) -> dict | None:
    """
    Download replay + run flycast for a single battle using the given working directory.
    Returns the parsed result dict or None.
    """
    pb_path = os.path.join(temp_dir, f"{battle_code}.pb")

    if not os.path.exists(pb_path):
        if not download_replay(replay_url, pb_path):
            return None

    result = run_flycast_batch(wdir, gdi_path, pb_path, pov=pov, timeout=timeout)

    try:
        if os.path.exists(pb_path):
            os.remove(pb_path)
    except OSError:
        pass

    return result


def compute_user_stats(result: dict) -> list[dict]:
    """Compute per-user stats from a batch result."""
    round_data = result.get("round_data", [])
    users = result.get("users", [])
    rounds = len(round_data)
    stats = []

    for user in users:
        user_id = user["user_id"]
        team = user["team"]
        pos = user["pos"]

        wins = sum(1 for rd in round_data if rd.get("win_team") == team)
        losses = rounds - wins
        round_win = ",".join(str(rd.get("win_team", 0)) for rd in round_data)

        user_ms_list = []
        for rd in round_data:
            used_ms = rd.get("used_ms", [])
            if pos - 1 < len(used_ms):
                user_ms_list.append(used_ms[pos - 1])
            else:
                user_ms_list.append(0)

        used_ms_list_str = ",".join(str(ms) for ms in user_ms_list)
        used_ms_mask = 0
        for ms in user_ms_list:
            if ms > 0:
                used_ms_mask |= 1 << ms

        stats.append({
            "battle_code": result["battle_code"],
            "user_id": user_id,
            "rounds": rounds,
            "wins": wins,
            "losses": losses,
            "round_win": round_win,
            "used_ms_list": used_ms_list_str,
            "used_ms_mask": used_ms_mask,
        })

    return stats


def update_db(conn: sqlite3.Connection, stats: list[dict]) -> int:
    """Update battle_record table with computed stats.
    round/win/lose are only set when not already recorded.
    round_win/used_ms_list/used_ms_mask are always updated.
    """
    if not stats:
        return 0

    updated = 0
    cursor = conn.cursor()

    for s in stats:
        cursor.execute(
            """
            UPDATE battle_record SET
                round = CASE WHEN round IS NULL OR round = 0 THEN ? ELSE round END,
                win = CASE WHEN round IS NULL OR round = 0 THEN ? ELSE win END,
                lose = CASE WHEN round IS NULL OR round = 0 THEN ? ELSE lose END,
                round_win = ?,
                used_ms_list = ?,
                used_ms_mask = ?
            WHERE battle_code = ? AND user_id = ?
            """,
            (s["rounds"], s["wins"], s["losses"], s["round_win"],
             s["used_ms_list"], s["used_ms_mask"], s["battle_code"], s["user_id"]),
        )
        updated += cursor.rowcount

    conn.commit()
    return updated


def write_sql_patch(sql_file, stats: list[dict]):
    """Append SQL UPDATE statements to the patch file."""
    for s in stats:
        # Escape single quotes in string values
        round_win = s["round_win"].replace("'", "''")
        used_ms_list = s["used_ms_list"].replace("'", "''")
        battle_code = s["battle_code"].replace("'", "''")
        user_id = s["user_id"].replace("'", "''")

        sql_file.write(
            f"UPDATE battle_record SET "
            f"round = CASE WHEN round IS NULL OR round = 0 THEN {s['rounds']} ELSE round END, "
            f"win = CASE WHEN round IS NULL OR round = 0 THEN {s['wins']} ELSE win END, "
            f"lose = CASE WHEN round IS NULL OR round = 0 THEN {s['losses']} ELSE lose END, "
            f"round_win = '{round_win}', "
            f"used_ms_list = '{used_ms_list}', "
            f"used_ms_mask = {s['used_ms_mask']} "
            f"WHERE battle_code = '{battle_code}' AND user_id = '{user_id}';\n"
        )
    sql_file.flush()


def get_pending_battles(
    conn: sqlite3.Connection,
    mode: str = "all",
    limit: int = 0,
) -> list[dict]:
    """Get battle records that need processing."""
    cursor = conn.cursor()

    if mode == "missing_round":
        condition = "(br.round IS NULL OR br.round = 0)"
    elif mode == "missing_ms":
        condition = "(br.used_ms_mask IS NULL OR br.used_ms_mask = 0)"
    else:
        condition = "(br.round IS NULL OR br.round = 0 OR br.used_ms_mask IS NULL OR br.used_ms_mask = 0)"

    query = f"""
        SELECT DISTINCT br.battle_code, br.replay_url
        FROM battle_record br
        WHERE br.replay_url IS NOT NULL
          AND br.replay_url != ''
          AND {condition}
        ORDER BY br.battle_code DESC
    """
    if limit > 0:
        query += f" LIMIT {limit}"

    cursor.execute(query)
    rows = cursor.fetchall()

    return [{"battle_code": row[0], "replay_url": row[1]} for row in rows]


def main():
    parser = argparse.ArgumentParser(description="Batch replay tool for GDXSV battle data extraction")
    parser.add_argument("--db", required=True, help="Path to gdxsv.db SQLite database")
    parser.add_argument("--flycast", required=True, help="Path to flycast executable")
    parser.add_argument("--gdi", required=True, help="Path to game .gdi file")
    parser.add_argument("--mode", choices=["all", "missing_round", "missing_ms"], default="all",
                        help="Which records to process (default: all)")
    parser.add_argument("--limit", type=int, default=0, help="Max battles to process (0=unlimited)")
    parser.add_argument("--timeout", type=int, default=300, help="Timeout per replay in seconds (default: 300)")
    parser.add_argument("--pov", type=int, default=1, help="Player POV for replay (default: 1)")
    parser.add_argument("--temp-dir", default=None, help="Directory for temporary .pb files")
    parser.add_argument("--dry-run", action="store_true", help="Don't update DB, just print results")
    parser.add_argument("--battle-code", default=None, help="Process a specific battle code only")
    parser.add_argument("--workers", type=int, default=1,
                        help="Number of parallel flycast processes (default: 1)")
    parser.add_argument("--sql-output", default=None,
                        help="Path to output SQL patch file recording all updates")
    args = parser.parse_args()

    if not os.path.exists(args.db):
        logger.error(f"Database not found: {args.db}")
        sys.exit(1)
    if not os.path.exists(args.flycast):
        logger.error(f"Flycast executable not found: {args.flycast}")
        sys.exit(1)
    if not os.path.exists(args.gdi):
        logger.error(f"GDI file not found: {args.gdi}")
        sys.exit(1)

    flycast_path = os.path.abspath(args.flycast)
    gdi_path = os.path.abspath(args.gdi)

    # Download savestates
    download_state()

    # Prepare one working directory per worker
    logger.info(f"Preparing {args.workers} working directories...")
    wdirs = []
    for i in range(args.workers):
        wdir = prepare_workdir(i + 1, flycast_path)
        wdirs.append(os.path.abspath(wdir))
        logger.info(f"  Worker {i + 1}: {wdir}")

    conn = sqlite3.connect(args.db)

    if args.battle_code:
        cursor = conn.cursor()
        cursor.execute(
            "SELECT DISTINCT battle_code, replay_url FROM battle_record "
            "WHERE battle_code = ? AND replay_url IS NOT NULL AND replay_url != ''",
            (args.battle_code,),
        )
        rows = cursor.fetchall()
        battles = [{"battle_code": row[0], "replay_url": row[1]} for row in rows]
    else:
        battles = get_pending_battles(conn, mode=args.mode, limit=args.limit)

    total = len(battles)
    logger.info(f"Found {total} battles to process (workers={args.workers})")

    if total == 0:
        conn.close()
        return

    temp_dir = args.temp_dir or tempfile.mkdtemp(prefix="gdxsv_batch_")
    os.makedirs(temp_dir, exist_ok=True)

    success_count = 0
    fail_count = 0
    skip_count = 0
    t_start = time.monotonic()

    sql_file = None
    if args.sql_output:
        sql_file = open(args.sql_output, "a", encoding="utf-8")
        sql_file.write(f"-- batch_replay.py run at {time.strftime('%Y-%m-%d %H:%M:%S')}\n")

    try:
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            future_to_battle = {}
            for i, battle in enumerate(battles):
                # Round-robin assign working directories to battles
                wdir = wdirs[i % args.workers]
                future = executor.submit(
                    process_one_battle,
                    battle["battle_code"],
                    battle["replay_url"],
                    wdir,
                    gdi_path,
                    args.pov,
                    args.timeout,
                    temp_dir,
                )
                future_to_battle[future] = battle

            done_count = 0
            for future in as_completed(future_to_battle):
                battle = future_to_battle[future]
                battle_code = battle["battle_code"]
                done_count += 1

                try:
                    result = future.result()
                except Exception as e:
                    logger.error(f"[{done_count}/{total}] battle_code={battle_code} exception: {e}")
                    fail_count += 1
                    continue

                if result is None:
                    logger.error(f"[{done_count}/{total}] battle_code={battle_code} failed")
                    fail_count += 1
                    continue

                if result.get("battle_code") != battle_code:
                    logger.error(
                        f"[{done_count}/{total}] Battle code mismatch: "
                        f"expected={battle_code}, got={result.get('battle_code')}"
                    )
                    fail_count += 1
                    continue

                round_data = result.get("round_data", [])
                if not round_data:
                    logger.warning(f"[{done_count}/{total}] battle_code={battle_code} no round data")
                    skip_count += 1
                    continue

                stats = compute_user_stats(result)

                elapsed = time.monotonic() - t_start
                rate = done_count / elapsed * 60 if elapsed > 0 else 0
                eta = (total - done_count) / (done_count / elapsed) if done_count > 0 and elapsed > 0 else 0

                logger.info(
                    f"[{done_count}/{total}] battle_code={battle_code} "
                    f"rounds={len(round_data)} "
                    f"win={','.join(str(rd.get('win_team', 0)) for rd in round_data)} "
                    f"({rate:.1f}/min, ETA {eta:.0f}s)"
                )

                if args.dry_run:
                    logger.info(f"  [DRY RUN] {json.dumps(result, ensure_ascii=False)}")
                    success_count += 1
                else:
                    rows_updated = update_db(conn, stats)
                    logger.info(f"  Updated {rows_updated} rows")
                    success_count += 1

                if sql_file:
                    write_sql_patch(sql_file, stats)
    finally:
        if sql_file:
            sql_file.close()

    # Clean up temp dir if we created it
    if not args.temp_dir:
        try:
            os.rmdir(temp_dir)
        except OSError:
            pass

    conn.close()

    elapsed = time.monotonic() - t_start
    logger.info(
        f"Done. success={success_count}, fail={fail_count}, skip={skip_count}, "
        f"total={total}, elapsed={elapsed:.1f}s"
    )
    if args.sql_output:
        logger.info(f"SQL patch written to: {args.sql_output}")


if __name__ == "__main__":
    main()
