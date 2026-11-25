"""Run the local STL replay parser and headless OpenBW worker."""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import random
import subprocess
import sys
from time import perf_counter


BIN_DIR = Path(__file__).resolve().parents[2] / "agent/output/stl-rep-cli"


def invoke(binary, args, timeout, stage, *, structured=True):
    started = perf_counter()
    try:
        child = subprocess.run(
            [str(binary), *map(str, args)], capture_output=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "stage": stage, "error": f"超过 {timeout:g} 秒"}
    except OSError as exc:
        return {"status": "process_error", "stage": stage, "error": str(exc)}

    if not structured and child.returncode == 0:
        return {"status": "ok", "data": child.stdout}
    if structured:
        try:
            result = json.loads(child.stdout)
            if isinstance(result, dict) and (
                child.returncode == 0 or result.get("status") in {"failed", "unsupported"}
            ):
                result["elapsed_ms"] = round((perf_counter() - started) * 1000)
                return result
        except (UnicodeError, ValueError):
            pass

    stderr = child.stderr.decode("utf-8", errors="replace").strip()
    return {
        "status": "failed" if stage == "parse" and child.returncode == 1 else "process_error",
        "stage": stage, "exit_code": child.returncode,
        "error": stderr[:2000] or "子进程没有返回可用数据",
    }


def compare(parsed, engine):
    differences = []
    if parsed["header"]["Frames"] != engine["end_frame"]:
        differences.append({
            "kind": "frame_count", "parser": parsed["header"]["Frames"],
            "engine": engine["end_frame"],
        })
    if engine["pending_action_bytes"]:
        differences.append({"kind": "unprocessed_actions", "bytes": engine["pending_action_bytes"]})
    if parsed["command_parse_errors"]:
        differences.append({"kind": "command_parse_errors", "count": parsed["command_parse_errors"]})
    players = {player["slot"]: player for player in engine["players"]}
    reported = {}
    for event in parsed["leave_events"]:
        slot = event.get("slot")
        if slot in players:
            reported.setdefault(slot, event["frame"])
    for slot, player in players.items():
        frame = (player["exit"]["frame"] if player["exit"] is not None
                 else player.get("exit_after_result_frame"))
        if frame != reported.get(slot):
            differences.append({
                "kind": "exit_frame", "slot": slot, "name": player["name"],
                "parser": reported.get(slot), "engine": frame,
            })
    return differences


def simulate_replay(path, args):
    try:
        with path.open("rb") as replay:
            header = replay.read(30)
    except OSError as exc:
        return {"status": "failed", "stage": "read_replay", "error": str(exc)}
    magic = header[12:16]
    if len(header) < 30 or magic not in {b"reRS", b"seRS"}:
        return {"status": "failed", "stage": "replay_format", "error": "无法识别为星际争霸录像"}
    # Match docs/v1.4/engine-loader.js, including 1.18–1.20 zlib headers.
    modern = magic == b"seRS" or (header[28] == 0x78 and int.from_bytes(header[28:30], "big") % 31 == 0)
    frame = args.frame if args.mode == "simulate" and args.frame is not None else -1
    engine_args = [args.data, path, frame]
    binary = args.engine_binary
    if modern:
        binary = args.remastered_binary
        if not binary.is_file():
            return {"status": "failed", "engine_type": "remastered", "stage": "engine_setup",
                    "error": f"找不到重制版引擎：{binary}"}
        if not args.terrain.is_file():
            return {"status": "failed", "engine_type": "remastered", "stage": "load_game_data",
                    "error": f"缺少重制版地形资源：{args.terrain}"}
        engine_args.append(args.terrain)
    if args.mode == "simulate" and args.units:
        engine_args.append("--units")
    result = invoke(binary, engine_args, args.timeout, "engine")
    result.setdefault("engine_type", "remastered" if modern else "classic")
    return result


def run_replay(path, args):
    result = {"path": str(path)}
    if args.mode in {"info", "check"}:
        parsed = invoke(args.parser_binary, ["info", path], args.timeout, "parse")
        parsed.setdefault("status", "ok")
        result["parser"] = parsed
    if args.mode in {"simulate", "check"}:
        engine = simulate_replay(path, args)
        if engine["status"] == "completed":
            players = engine["players"]
            winners = [player["slot"] for player in players if player["engine_win"]]
            engine["winner_slots"] = winners
            engine["winner_source"] = "engine" if winners else None
            active = [player["slot"] for player in players if player["active"]]
            if not winners and len(players) == 2 and len(active) == 1 and any(
                player["exit"] and player["exit"]["kind"] == "leave"
                and player["slot"] != active[0] for player in players
            ):
                engine["winner_slots"] = active
                engine["winner_source"] = "last_player_remaining"
        result["engine"] = engine
    if args.mode == "info":
        result["status"] = parsed["status"]
    elif args.mode == "simulate":
        result["status"] = engine["status"]
    elif parsed["status"] != "ok":
        result["status"] = f"parser_{parsed['status']}"
    elif engine["status"] != "completed":
        result["status"] = engine["status"]
    else:
        result["differences"] = compare(parsed, engine)
        result["status"] = "mismatch" if result["differences"] else "completed"
    return result


def replay_files(paths):
    files = {}
    for path in paths:
        if path.is_dir():
            candidates = (entry for entry in path.rglob("*") if entry.is_file())
        elif path.is_file() and path.suffix.lower() == ".rep":
            candidates = [path]
        else:
            raise ValueError(f"找不到 REP 文件或目录：{path}")
        for entry in candidates:
            if entry.suffix.lower() == ".rep":
                resolved = entry.resolve()
                files[os.path.normcase(resolved)] = resolved
    return [files[key] for key in sorted(files)]


def json_line(record, output):
    print(json.dumps(record, ensure_ascii=False, separators=(",", ":")), file=output)


def batch(args):
    paths = replay_files(args.paths)
    if not paths:
        raise ValueError("没有找到 REP 文件")
    if args.sample:
        paths = sorted(random.Random(args.seed).sample(paths, min(args.sample, len(paths))))
    output = args.output.open("x", encoding="utf-8") if args.output else sys.stdout
    counts = Counter()
    parsed = 0
    finished = Counter()
    engine_types = Counter()
    try:
        with ThreadPoolExecutor(max_workers=args.jobs) as executor:
            for result in executor.map(lambda path: run_replay(path, args), paths):
                counts[result["status"]] += 1
                if result.get("parser", {}).get("status") == "ok":
                    parsed += 1
                engine = result.get("engine", {})
                if engine_type := engine.get("engine_type"):
                    engine_types[engine_type] += 1
                    if engine["status"] == "completed":
                        finished[engine_type] += 1
                json_line(result, output)
    finally:
        if args.output:
            output.close()
    summary = {"total": len(paths), "statuses": dict(counts)}
    if args.mode in {"info", "check"}:
        summary["parsed"] = parsed
    if args.mode in {"simulate", "check"}:
        summary.update({"finished": sum(finished.values()), "engine_types": dict(engine_types),
                        "finished_by_engine": dict(finished)})
    json_line(summary, sys.stderr)
    return int(any(status not in {"ok", "completed", "checkpoint"} for status in counts))


def single(args):
    if not args.path.is_file() or args.path.suffix.lower() != ".rep":
        raise ValueError(f"找不到 REP 文件：{args.path}")
    mode = "commands" if args.mode == "build-order" else args.mode
    result = invoke(args.parser_binary, [mode, args.path], args.timeout, "parse", structured=False)
    if result["status"] != "ok":
        json_line(result, sys.stderr)
        return 1
    if mode == "extract-map":
        with args.output.open("xb") as output:
            output.write(result["data"])
        return 0
    for line in result["data"].splitlines():
        event = json.loads(line)
        if args.mode == "build-order" and not event["production"]:
            continue
        if args.player is not None and event["player_id"] != args.player:
            continue
        if args.type and not any(term.casefold() in event["type"].casefold() for term in args.type):
            continue
        if args.from_frame is not None and event["frame"] < args.from_frame:
            continue
        if args.to_frame is not None and event["frame"] > args.to_frame:
            continue
        json_line(event, sys.stdout)
    return 0


def positive_int(text):
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("需要正整数")
    return value


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    cli = argparse.ArgumentParser(description="STL 本地 REP 解析、模拟与批量诊断")
    sub = cli.add_subparsers(dest="mode", required=True)
    descriptions = {
        "info": "提取地图、玩家、时长、APM、退场和胜队估计",
        "simulate": "用 OpenBW 逐帧模拟经典版或重制版录像",
        "check": "批量对比 REP 解析结果与 OpenBW 模拟结果",
        "commands": "导出录像中的玩家指令",
        "build-order": "导出建造、生产、变形、研究和升级指令",
        "extract-map": "导出录像内嵌的 CHK 地图",
    }
    for mode, description in descriptions.items():
        command = sub.add_parser(mode, help=description, description=description)
        command.add_argument("--bin-dir", type=Path, default=BIN_DIR, help="本地工具可执行文件目录")
        command.add_argument("--timeout", type=positive_int, default=60, help="每个子进程超时秒数")
        if mode in {"info", "simulate", "check"}:
            command.add_argument("paths", type=Path, nargs="+", help="REP 文件或递归扫描的目录")
            command.add_argument("--output", type=Path, help="另存 JSONL（须为新文件）")
            command.add_argument("--sample", type=positive_int, help="从输入文件中随机抽样")
            command.add_argument("--seed", type=int, default=0, help="抽样随机种子")
            command.add_argument("--jobs", type=positive_int, default=1, help="并行处理录像数")
            if mode != "info":
                command.add_argument("--data", required=True, type=Path, help="三个经典版 MPQ 所在目录")
                command.add_argument("--terrain", type=Path, help="重制版地形 MPQ，默认读取数据目录下的 terrain.mpq")
            if mode == "simulate":
                command.add_argument("--frame", type=int, help="停在指定帧，输出该帧的玩家状态")
                command.add_argument("--units", action="store_true", help="同时输出参与玩家的单位快照")
        else:
            command.add_argument("path", type=Path, help="REP 文件")
            if mode == "extract-map":
                command.add_argument("--output", required=True, type=Path, help="新建 CHK 文件")
            else:
                command.add_argument("--player", type=int, help="录像指令中的玩家 ID，见 info")
                command.add_argument("--type", action="append", help="按指令类型子串筛选，可重复")
                command.add_argument("--from-frame", type=int)
                command.add_argument("--to-frame", type=int)

    args = cli.parse_args()
    if args.mode == "simulate" and args.frame is not None and not 0 <= args.frame <= 2147483647:
        cli.error("--frame 必须在 0 到 2147483647 之间")
    args.parser_binary = args.bin_dir / "stl-rep-parser.exe"
    args.engine_binary = args.bin_dir / "stl-rep-engine.exe"
    args.remastered_binary = args.bin_dir / "stl-rep-engine-remastered.exe"
    if args.mode != "simulate" and not args.parser_binary.is_file():
        cli.error(f"找不到解析工具：{args.parser_binary}")
    if args.mode in {"simulate", "check"}:
        args.terrain = args.terrain or args.data / "terrain.mpq"
        if not args.engine_binary.is_file():
            cli.error(f"找不到模拟引擎：{args.engine_binary}")
        for mpq in ("StarDat.mpq", "BrooDat.mpq", "patch_rt.mpq"):
            if not (args.data / mpq).is_file():
                cli.error(f"缺少经典版资源：{args.data / mpq}")
    try:
        if args.mode in {"info", "simulate", "check"}:
            return batch(args)
        if args.mode in {"commands", "build-order"}:
            if args.from_frame is not None and args.to_frame is not None and args.from_frame > args.to_frame:
                cli.error("--from-frame 不能大于 --to-frame")
        return single(args)
    except (OSError, ValueError) as error:
        cli.error(str(error))


if __name__ == "__main__":
    sys.exit(main())
