from __future__ import annotations

import argparse
from pathlib import Path
from typing import List, Tuple

ROWS = 40
COLS = 60


def parse_dvr_csv(path: Path, rows: int = ROWS, cols: int = COLS) -> List[List[List[int]]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    frames: List[List[List[int]]] = []
    i = 0
    while i < len(lines):
        if lines[i].startswith("--- Frame"):
            while i < len(lines) and lines[i].strip() != "Heatmap:":
                i += 1
            if i >= len(lines):
                break
            i += 1
            if i + rows > len(lines):
                break

            mat: List[List[int]] = []
            ok = True
            for r in range(rows):
                parts = lines[i + r].strip().split(",")
                if len(parts) != cols:
                    ok = False
                    break
                try:
                    mat.append([int(x) for x in parts])
                except ValueError:
                    ok = False
                    break
            if ok:
                frames.append(mat)
            i += rows
        else:
            i += 1
    return frames


def frame_stats(frame: List[List[int]], threshold: int = 150) -> dict:
    flat = [v for row in frame for v in row]
    peak = max(flat) if flat else 0
    total = sum(flat)
    active = sum(1 for v in flat if v >= threshold)
    return {
        "peak": peak,
        "sum": total,
        "active_pixels": active,
        "mean": (total / len(flat)) if flat else 0.0,
    }


def top_points(frame: List[List[int]], k: int = 10) -> List[Tuple[int, int, int]]:
    pts: List[Tuple[int, int, int]] = []
    for y in range(len(frame)):
        for x in range(len(frame[y])):
            pts.append((y, x, frame[y][x]))
    pts.sort(key=lambda p: p[2], reverse=True)
    return pts[:k]


def print_frame_ascii(frame: List[List[int]], width: int = 60) -> None:
    chars = " .:-=+*#%@"
    vmax = max(max(row) for row in frame) if frame else 1
    vmax = max(vmax, 1)
    for row in frame:
        line = []
        for v in row[:width]:
            idx = int((v / vmax) * (len(chars) - 1))
            line.append(chars[idx])
        print("".join(line))


def resolve_dvr(base: Path, dvr_name_or_path: str) -> Path:
    p = Path(dvr_name_or_path)
    if p.exists():
        return p

    candidate = base / "Python" / "data" / "exports" / "dvr" / dvr_name_or_path
    if candidate.exists():
        return candidate

    candidate_csv = candidate.with_suffix(".csv")
    if candidate_csv.exists():
        return candidate_csv

    raise FileNotFoundError(f"DVR 文件不存在: {dvr_name_or_path}")


def cmd_summary(args: argparse.Namespace) -> None:
    path = resolve_dvr(args.repo_root, args.dvr)
    frames = parse_dvr_csv(path)
    print(f"dvr: {path}")
    print(f"frames: {len(frames)}")
    if not frames:
        return

    peaks = [frame_stats(f, args.threshold)["peak"] for f in frames]
    active = [frame_stats(f, args.threshold)["active_pixels"] for f in frames]
    print(f"peak min/max/avg: {min(peaks)} / {max(peaks)} / {sum(peaks)/len(peaks):.2f}")
    print(f"active_pixels avg: {sum(active)/len(active):.2f}")


def cmd_frame(args: argparse.Namespace) -> None:
    path = resolve_dvr(args.repo_root, args.dvr)
    frames = parse_dvr_csv(path)
    if not frames:
        raise RuntimeError("没有可读取帧")

    idx = args.index
    if idx < 0:
        idx += len(frames)
    if idx < 0 or idx >= len(frames):
        raise IndexError(f"帧索引越界: {args.index}, 总帧数={len(frames)}")

    frame = frames[idx]
    stats = frame_stats(frame, args.threshold)
    print(f"dvr: {path.name}")
    print(f"frame: {idx}/{len(frames)-1}")
    print(f"stats: {stats}")

    if args.topk > 0:
        print(f"top{args.topk} points (y, x, value):")
        for pt in top_points(frame, args.topk):
            print(" ", pt)

    if args.ascii:
        print("ascii preview:")
        print_frame_ascii(frame)


def cmd_replay(args: argparse.Namespace) -> None:
    path = resolve_dvr(args.repo_root, args.dvr)
    frames = parse_dvr_csv(path)
    if not frames:
        raise RuntimeError("没有可读取帧")

    start = max(0, args.start)
    end = min(len(frames), args.end if args.end is not None else len(frames))
    step = max(1, args.step)

    print(f"replay dvr: {path.name}, range=[{start}, {end}), step={step}")
    for i in range(start, end, step):
        s = frame_stats(frames[i], args.threshold)
        print(f"frame={i:04d} peak={s['peak']:5d} active={s['active_pixels']:4d} sum={s['sum']:7d}")


def cmd_extract(args: argparse.Namespace) -> None:
    path = resolve_dvr(args.repo_root, args.dvr)
    frames = parse_dvr_csv(path)
    if not frames:
        raise RuntimeError("没有可读取帧")

    idx = args.index
    if idx < 0:
        idx += len(frames)
    if idx < 0 or idx >= len(frames):
        raise IndexError(f"帧索引越界: {args.index}, 总帧数={len(frames)}")

    frame = frames[idx]
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8") as f:
        for row in frame:
            f.write(",".join(str(v) for v in row) + "\n")
    print(f"saved frame {idx} to: {out}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="DVR debug tools: replay / inspect frame / extract frame")
    parser.add_argument("--repo-root", type=Path, default=Path.cwd(), help="repo root path")
    parser.add_argument("--threshold", type=int, default=150, help="active pixel threshold")

    sub = parser.add_subparsers(dest="cmd", required=True)

    p_summary = sub.add_parser("summary", help="show DVR summary")
    p_summary.add_argument("dvr", help="dvr file path or filename")
    p_summary.set_defaults(func=cmd_summary)

    p_frame = sub.add_parser("frame", help="inspect one specific frame")
    p_frame.add_argument("dvr", help="dvr file path or filename")
    p_frame.add_argument("index", type=int, help="frame index (supports negative index)")
    p_frame.add_argument("--topk", type=int, default=10, help="print top-k points")
    p_frame.add_argument("--ascii", action="store_true", help="print ascii heatmap")
    p_frame.set_defaults(func=cmd_frame)

    p_replay = sub.add_parser("replay", help="replay frame stats in range")
    p_replay.add_argument("dvr", help="dvr file path or filename")
    p_replay.add_argument("--start", type=int, default=0)
    p_replay.add_argument("--end", type=int, default=None)
    p_replay.add_argument("--step", type=int, default=1)
    p_replay.set_defaults(func=cmd_replay)

    p_extract = sub.add_parser("extract", help="extract one frame to csv")
    p_extract.add_argument("dvr", help="dvr file path or filename")
    p_extract.add_argument("index", type=int, help="frame index")
    p_extract.add_argument("--output", required=True, help="output csv file path")
    p_extract.set_defaults(func=cmd_extract)

    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
