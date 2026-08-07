import argparse
import csv
import hashlib
import math
import re
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


POSE_COLS = ["Lon", "Lat", "Alt", "Roll", "Pitch", "Yaw"]
ANGLE_COLS = ["Roll", "Pitch", "Yaw"]
OUTPUT_TRACK_DIRNAME = "UE5_Split_Data"
TIME_ROUND_DECIMALS = 6
STATIC_EPSILON = 1e-4
STATIC_FRAME_COUNT = 3


def infer_category(obj_type: str) -> str:
    upper = str(obj_type).upper()
    if "EXPLOSION" in upper or "DEBRIS" in upper:
        return "ExplosionFX"
    if "MISSILE" in upper or "AIM" in upper or "WEAPON" in upper:
        return "Missile"
    return "Plane"


def infer_team(color: str, coalition: str = "") -> str:
    combined = f"{color} {coalition}".upper()
    if "BLUE" in combined or "ALLIES" in combined or "FRIEND" in combined:
        return "Blue"
    if "RED" in combined or "ENEM" in combined or "HOSTILE" in combined:
        return "Red"
    return color if color and color != "Unknown" else (coalition or "Unknown")


def round_time(value: float) -> float:
    return round(float(value), TIME_ROUND_DECIMALS)


def parse_acmi(acmi_file_path: Path):
    print(f"Parsing ACMI: {acmi_file_path}")

    all_timestamps = set()
    objects_meta: Dict[str, Dict[str, str]] = {}
    raw_data: List[Dict[str, object]] = []
    removal_events: Dict[str, float] = {}
    current_time = 0.0

    with acmi_file_path.open("r", encoding="utf-8-sig") as source:
        for raw_line in source:
            line = raw_line.strip()
            if not line:
                continue

            if line.startswith("#"):
                try:
                    current_time = round_time(float(line[1:]))
                    all_timestamps.add(current_time)
                except ValueError:
                    pass
                continue

            if line.startswith("-"):
                removed_id = line.split(",", 1)[0][1:]
                removal_events.setdefault(removed_id, current_time)
                continue

            if "," not in line:
                continue

            parts = line.split(",")
            obj_id = parts[0]
            meta = objects_meta.setdefault(
                obj_id,
                {
                    "Type": "Unknown",
                    "Color": "Unknown",
                    "Coalition": "",
                    "Category": "Plane",
                },
            )

            object_name = None
            for part in parts[1:]:
                if part.startswith("Type="):
                    meta["Type"] = part.split("=", 1)[1]
                elif part.startswith("Name="):
                    object_name = part.split("=", 1)[1]
                elif part.startswith("Color="):
                    meta["Color"] = part.split("=", 1)[1]
                elif part.startswith("Coalition="):
                    meta["Coalition"] = part.split("=", 1)[1]
            if meta["Type"] == "Unknown" and object_name:
                meta["Type"] = object_name
            meta["Category"] = infer_category(meta["Type"])

            t_part = next((part for part in parts[1:] if part.startswith("T=")), None)
            if t_part is None or meta["Category"] == "ExplosionFX":
                continue

            coords = t_part[2:].split("|")
            if len(coords) < 3:
                continue

            try:
                raw_data.append(
                    {
                        "Time": current_time,
                        "ID": obj_id,
                        "Lon": float(coords[0]),
                        "Lat": float(coords[1]),
                        "Alt": float(coords[2]),
                        "Roll": float(coords[3]) if len(coords) > 3 else 0.0,
                        "Pitch": float(coords[4]) if len(coords) > 4 else 0.0,
                        "Yaw": float(coords[5]) if len(coords) > 5 else 0.0,
                    }
                )
            except ValueError:
                continue

    if not all_timestamps:
        raise ValueError(f"No ACMI timestamps found in {acmi_file_path}")
    if not raw_data:
        raise ValueError(f"No usable trajectory rows found in {acmi_file_path}")

    return sorted(all_timestamps), objects_meta, raw_data, removal_events


def estimate_source_dt(sorted_times: Sequence[float]) -> float:
    positive_diffs = [
        later - earlier
        for earlier, later in zip(sorted_times, sorted_times[1:])
        if later - earlier > 1e-8
    ]
    return min(positive_diffs) if positive_diffs else 0.5


def build_resampled_timeline(sorted_times: Sequence[float], target_dt: float) -> List[float]:
    if target_dt <= 0.0:
        raise ValueError(f"target_dt must be positive, got {target_dt}")

    start_time = round_time(sorted_times[0])
    end_time = round_time(sorted_times[-1])
    dense_count = int(math.floor((end_time - start_time) / target_dt + 1e-9)) + 1
    dense_times = [round_time(start_time + index * target_dt) for index in range(dense_count)]
    if not dense_times or dense_times[-1] < end_time - 1e-8:
        dense_times.append(end_time)
    return sorted({round_time(value) for value in [*sorted_times, *dense_times]})


def interpolate_series(
    times_src: Sequence[float],
    values_src: Sequence[float],
    times_dst: Sequence[float],
) -> List[Optional[float]]:
    output: List[Optional[float]] = [None] * len(times_dst)
    if not times_src:
        return output
    if len(times_src) == 1:
        for index, value in enumerate(times_dst):
            if math.isclose(value, times_src[0], abs_tol=1e-8):
                output[index] = float(values_src[0])
        return output

    source_index = 0
    for destination_index, destination_time in enumerate(times_dst):
        if destination_time < times_src[0] - 1e-8 or destination_time > times_src[-1] + 1e-8:
            continue
        while source_index + 1 < len(times_src) and times_src[source_index + 1] < destination_time - 1e-8:
            source_index += 1
        if source_index + 1 >= len(times_src):
            output[destination_index] = float(values_src[-1])
            continue
        left_time = times_src[source_index]
        right_time = times_src[source_index + 1]
        if math.isclose(destination_time, left_time, abs_tol=1e-8):
            output[destination_index] = float(values_src[source_index])
        elif math.isclose(destination_time, right_time, abs_tol=1e-8):
            output[destination_index] = float(values_src[source_index + 1])
        else:
            alpha = (destination_time - left_time) / (right_time - left_time)
            output[destination_index] = (
                float(values_src[source_index]) * (1.0 - alpha)
                + float(values_src[source_index + 1]) * alpha
            )
    return output


def unwrap_degrees(values: Sequence[float]) -> List[float]:
    if not values:
        return []
    unwrapped = [float(values[0])]
    previous_raw = float(values[0])
    for value in values[1:]:
        raw = float(value)
        delta = (raw - previous_raw + 180.0) % 360.0 - 180.0
        unwrapped.append(unwrapped[-1] + delta)
        previous_raw = raw
    return unwrapped


def infer_static_plane_death_time(rows: Sequence[Dict[str, object]]) -> Optional[float]:
    if len(rows) < STATIC_FRAME_COUNT:
        return None
    static_run = 0
    for index in range(len(rows) - 1):
        pose_delta = sum(
            abs(float(rows[index + 1][column]) - float(rows[index][column]))
            for column in POSE_COLS
        )
        static_run = static_run + 1 if pose_delta < STATIC_EPSILON else 0
        if static_run >= STATIC_FRAME_COUNT - 1:
            start_index = index - (STATIC_FRAME_COUNT - 2)
            return float(rows[start_index]["Time"])
    return None


def infer_death_time(
    obj_id: str,
    category: str,
    rows: Sequence[Dict[str, object]],
    removal_events: Dict[str, float],
) -> Tuple[Optional[float], bool]:
    if obj_id in removal_events:
        return round_time(removal_events[obj_id]), True
    if category == "Plane":
        static_death_time = infer_static_plane_death_time(rows)
        if static_death_time is not None:
            return round_time(static_death_time), True
    return None, False


def smooth_values(values: List[Optional[float]], valid_indices: Sequence[int], window: int) -> None:
    if window <= 1 or not valid_indices:
        return
    original = values[:]
    half_left = (window - 1) // 2
    half_right = window // 2
    first_valid = valid_indices[0]
    last_valid = valid_indices[-1]
    for index in valid_indices:
        samples = [
            original[sample_index]
            for sample_index in range(
                max(first_valid, index - half_left),
                min(last_valid, index + half_right) + 1,
            )
            if original[sample_index] is not None
        ]
        if samples:
            values[index] = sum(float(sample) for sample in samples) / len(samples)


def build_object_rows(
    obj_id: str,
    meta: Dict[str, str],
    raw_data: Sequence[Dict[str, object]],
    timeline: Sequence[float],
    removal_events: Dict[str, float],
    smoothing_window: int,
) -> Optional[List[Dict[str, object]]]:
    by_time = {
        float(row["Time"]): row
        for row in raw_data
        if row["ID"] == obj_id
    }
    source_rows = [by_time[key] for key in sorted(by_time)]
    if not source_rows:
        return None

    times_src = [float(row["Time"]) for row in source_rows]
    columns: Dict[str, List[Optional[float]]] = {}
    for column in POSE_COLS:
        source_values = [float(row[column]) for row in source_rows]
        if column in ANGLE_COLS:
            source_values = unwrap_degrees(source_values)
        columns[column] = interpolate_series(times_src, source_values, timeline)

    birth_time = times_src[0]
    last_raw_time = times_src[-1]
    death_time, should_explode = infer_death_time(
        obj_id, meta["Category"], source_rows, removal_events
    )
    extension_end_time = death_time if death_time is not None else last_raw_time
    if extension_end_time > last_raw_time + 1e-8:
        for column in POSE_COLS:
            last_value = float(source_rows[-1][column])
            for index, value in enumerate(timeline):
                if last_raw_time < value <= extension_end_time + 1e-8:
                    columns[column][index] = last_value

    valid_indices = [
        index
        for index, value in enumerate(timeline)
        if birth_time - 1e-8 <= value <= extension_end_time + 1e-8
    ]
    for column in POSE_COLS:
        smooth_values(columns[column], valid_indices, smoothing_window)
        last_value: Optional[float] = None
        for index, value in enumerate(columns[column]):
            if value is not None:
                last_value = value
            elif last_value is not None:
                columns[column][index] = last_value
        columns[column] = [0.0 if value is None else value for value in columns[column]]

    explosion_index = None
    if death_time is not None and should_explode:
        explosion_index = next(
            (index for index, value in enumerate(timeline) if value >= death_time - 1e-8),
            None,
        )

    output_rows = []
    for index, value in enumerate(timeline):
        if death_time is None:
            active = birth_time - 1e-8 <= value <= last_raw_time + 1e-8
        else:
            active = birth_time - 1e-8 <= value < death_time - 1e-8
        output_rows.append(
            {
                "FrameID": index,
                "Time": value,
                "Active": int(active),
                "Explosion": int(index == explosion_index),
                **{column: columns[column][index] for column in POSE_COLS},
            }
        )
    return output_rows


def safe_filename_component(value: str) -> str:
    safe_value = re.sub(r"[^A-Za-z0-9_.-]", "_", value)
    if safe_value == value and safe_value:
        return safe_value
    digest = hashlib.sha1(value.encode("utf-8")).hexdigest()[:8]
    return f"{safe_value or 'entity'}_{digest}"


def write_csv(path: Path, fieldnames: Sequence[str], rows: Sequence[Dict[str, object]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows({field: row[field] for field in fieldnames} for row in rows)


def write_object_csvs(
    output_dir: Path,
    obj_id: str,
    meta: Dict[str, str],
    rows: Sequence[Dict[str, object]],
) -> Dict[str, str]:
    category = meta["Category"]
    file_id = safe_filename_component(obj_id)
    track_filename = f"Track_{category}_{file_id}.csv"
    explosion_filename = f"Track_{category}_Explosion_{file_id}.csv"
    pose_fields = ["FrameID", "Time", "Lon", "Lat", "Alt", "Roll", "Pitch", "Yaw"]
    write_csv(output_dir / track_filename, ["FrameID", "Time", "Active", *pose_fields[2:]], rows)
    write_csv(
        output_dir / explosion_filename,
        ["FrameID", "Time", "Explosion", *pose_fields[2:]],
        rows,
    )
    return {
        "ID": obj_id,
        "Category": category,
        "Type": meta["Type"],
        "Team": infer_team(meta["Color"], meta.get("Coalition", "")),
        "TrackFile": track_filename,
        "ExplosionFile": explosion_filename,
    }


def parse_acmi_and_generate_ue5_data(
    acmi_file_path: str,
    output_dir: str = OUTPUT_TRACK_DIRNAME,
    target_dt: float = 0.1,
    smoothing_window: int = 0,
):
    acmi_path = Path(acmi_file_path).expanduser().resolve()
    out_dir = Path(output_dir).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    sorted_times, objects_meta, raw_data, removal_events = parse_acmi(acmi_path)
    source_dt = estimate_source_dt(sorted_times)
    timeline = build_resampled_timeline(sorted_times, target_dt=target_dt)
    print(f"Source frames: {len(sorted_times)}, source_dt ~= {source_dt:.4f}s")
    print(f"Resampled frames: {len(timeline)}, target_dt = {target_dt:.4f}s")

    manifest_rows = []
    for obj_id, meta in objects_meta.items():
        if meta["Category"] not in ("Plane", "Missile"):
            continue
        rows = build_object_rows(
            obj_id,
            meta,
            raw_data,
            timeline,
            removal_events,
            smoothing_window,
        )
        if rows:
            manifest_rows.append(write_object_csvs(out_dir, obj_id, meta, rows))

    if not manifest_rows:
        raise ValueError("No Plane or Missile tracks were exported from the ACMI file.")
    write_csv(
        out_dir / "Match_Manifest.csv",
        ["ID", "Category", "Type", "Team", "TrackFile", "ExplosionFile"],
        manifest_rows,
    )
    print(f"Manifest written: {out_dir / 'Match_Manifest.csv'}")
    print(f"UE5 CSV export complete: {out_dir}")
    return out_dir


def parse_args():
    default_acmi = Path(
        r"renders\optional_maspo_vs_expert_rule_annihilation\optional_maspo_actor_latest_46_vs_expert_rule.txt.acmi"
    )
    parser = argparse.ArgumentParser(description="Convert ACMI to denser UE5-friendly CSV tracks.")
    parser.add_argument("--path", default=str(default_acmi), help="Path to the ACMI file.")
    parser.add_argument(
        "--output-dir",
        default=OUTPUT_TRACK_DIRNAME,
        help="Directory to write UE5 CSV files.",
    )
    parser.add_argument(
        "--target-dt",
        type=float,
        default=0.1,
        help="Resampled timestep in seconds. Smaller means denser CSV.",
    )
    parser.add_argument(
        "--smoothing-window",
        type=int,
        default=0,
        help="Optional centered moving-average window. 0 disables smoothing.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    acmi_path = Path(args.path).expanduser()
    if not acmi_path.exists():
        raise FileNotFoundError(f"ACMI file not found: {acmi_path}")
    parse_acmi_and_generate_ue5_data(
        acmi_file_path=str(acmi_path),
        output_dir=args.output_dir,
        target_dt=args.target_dt,
        smoothing_window=args.smoothing_window,
    )


if __name__ == "__main__":
    main()
