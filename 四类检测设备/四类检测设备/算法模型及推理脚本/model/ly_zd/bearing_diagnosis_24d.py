#!/usr/bin/env python3
"""24D four-class diagnosis runner for CSV and C watch modes."""

import argparse
import json
import os
import time

import numpy as np
import onnxruntime as ort


RAW_POINTS = 12
FEATURE_DIM = 24
VOLTAGE_MAX = 8.0
VOLTAGE_CLAMP_MAX = 10.0
BEAKER_MAX = 500.0
MU_LAW = 255.0
DIFF_SCALE = 720.0

DEFAULT_MODEL = "/home/root/model/ly_zd/model/diagnosis_model.onnx"
DEFAULT_METADATA = "/home/root/model/ly_zd/model/metadata.json"
DEFAULT_SCALER = "/home/root/model/ly_zd/model/scaler.json"
DEFAULT_CLASSES = ["normal", "mild", "moderate", "severe"]


def load_json(path):
    last_error = None
    for encoding in ("utf-8-sig", "gb18030"):
        try:
            with open(path, "r", encoding=encoding) as f:
                return json.load(f)
        except UnicodeDecodeError as exc:
            last_error = exc
    raise last_error


def first_value(mapping, keys, default=None):
    if not isinstance(mapping, dict):
        return default
    for key in keys:
        if key in mapping and mapping[key] is not None:
            return mapping[key]
    for value in mapping.values():
        if isinstance(value, dict):
            found = first_value(value, keys, None)
            if found is not None:
                return found
    return default


def as_stat_row(value, name):
    arr = np.asarray(value, dtype=np.float32).reshape(1, -1)
    if arr.shape[1] != FEATURE_DIM:
        raise ValueError(f"{name} must contain {FEATURE_DIM} values, got {arr.shape[1]}")
    return arr


def load_config(metadata_path, scaler_path):
    metadata = load_json(metadata_path)
    scaler = load_json(scaler_path)

    mean = first_value(scaler, ["mean", "mean_", "scaler_mean"])
    scale = first_value(scaler, ["scale", "scale_", "std", "std_", "scaler_scale"])
    if mean is None:
        mean = first_value(metadata, ["mean", "mean_", "scaler_mean"])
    if scale is None:
        scale = first_value(metadata, ["scale", "scale_", "std", "std_", "scaler_scale"])
    if mean is None or scale is None:
        raise ValueError("mean/scale not found in scaler.json or metadata.json")

    class_names = first_value(
        metadata,
        ["class_names", "classes", "label_names", "labels"],
        DEFAULT_CLASSES,
    )
    if isinstance(class_names, dict):
        class_names = [class_names[str(i)] for i in range(len(class_names))]
    class_names = [str(name) for name in class_names]

    mean = as_stat_row(mean, "mean")
    scale = as_stat_row(scale, "scale")
    scale = np.where(scale == 0.0, 1.0, scale).astype(np.float32)
    return mean, scale, class_names


def voltage_to_beaker(values):
    voltage = np.asarray(values, dtype=np.float32)
    voltage = np.clip(voltage, 0.0, VOLTAGE_CLAMP_MAX)
    beaker = BEAKER_MAX * (np.power(1.0 + MU_LAW, voltage / VOLTAGE_MAX) - 1.0) / MU_LAW
    return beaker.astype(np.float32)


def voltage_to_features24(values, input_type="voltage"):
    values = np.asarray(values, dtype=np.float32).reshape(-1)
    if values.size == 0 or values.size % RAW_POINTS != 0:
        raise ValueError(f"input count {values.size} must be a positive multiple of {RAW_POINTS}")

    samples = values.reshape(-1, RAW_POINTS)
    if input_type == "voltage":
        beaker = voltage_to_beaker(samples)
    elif input_type == "beaker":
        beaker = samples
    else:
        raise ValueError(f"unsupported input type: {input_type}")
    diff_raw = np.diff(beaker, axis=1)
    diff_raw = np.where(diff_raw < -100.0, diff_raw + 500.0, diff_raw)
    diff = np.concatenate(
        [np.zeros((beaker.shape[0], 1), dtype=np.float32), diff_raw], axis=1
    )
    diff *= DIFF_SCALE
    return np.concatenate([beaker, diff], axis=1).astype(np.float32)


def create_session(model_path):
    session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    input_info = session.get_inputs()[0]
    output_info = session.get_outputs()[0]
    print(f"Model: {model_path}")
    print(f"Input: {input_info.name} {input_info.shape}")
    print(f"Output: {output_info.name} {output_info.shape}")
    return session, input_info.name, output_info.name


def softmax(logits):
    shifted = logits - np.max(logits, axis=1, keepdims=True)
    exp = np.exp(shifted)
    return exp / np.sum(exp, axis=1, keepdims=True)


def infer_batch(session, input_name, output_name, features, mean, scale, class_names):
    features = np.asarray(features, dtype=np.float32).reshape(-1, FEATURE_DIM)
    scaled = ((features - mean) / scale).astype(np.float32)
    logits = session.run([output_name], {input_name: scaled})[0]
    logits = np.asarray(logits, dtype=np.float32).reshape(len(features), -1)
    if logits.shape[1] != len(class_names):
        raise ValueError(
            f"model returns {logits.shape[1]} classes but metadata defines {len(class_names)}"
        )
    probabilities = softmax(logits)
    pred_ids = np.argmax(probabilities, axis=1).astype(np.int32)
    counts = np.bincount(pred_ids, minlength=len(class_names))
    final_id = int(np.argmax(counts))
    return pred_ids, probabilities, counts, final_id


def read_csv_voltage(path, has_header=False):
    data = np.loadtxt(
        path,
        delimiter=",",
        skiprows=1 if has_header else 0,
        dtype=np.float32,
    )
    return np.asarray(data, dtype=np.float32).reshape(-1)


def read_bin_voltage(path, max_ch, max_points, channel, offset, count):
    raw = np.fromfile(path, dtype=np.float32)
    expected = max_ch * max_points
    if raw.size != expected:
        raise ValueError(f"binary file has {raw.size} float32 values, expected {expected}")
    if channel < 0 or channel >= max_ch:
        raise ValueError(f"channel {channel} is outside 0..{max_ch - 1}")
    if offset < 0 or offset + count > max_points:
        raise ValueError("requested voltage window exceeds max_points")
    return raw.reshape(max_ch, max_points)[channel, offset : offset + count].copy()


def print_result(pred_ids, probabilities, counts, final_id, class_names):
    for i, pred_id in enumerate(pred_ids, 1):
        confidence = float(probabilities[i - 1, pred_id])
        print(
            f"sample {i:02d}: class={pred_id} ({class_names[pred_id]}), "
            f"confidence={confidence:.6f}"
        )
    if len(pred_ids) > 1:
        count_text = ", ".join(
            f"{i}:{class_names[i]}={int(count)}" for i, count in enumerate(counts)
        )
        print(f"vote: {count_text}")
    print(f"final: {final_id} ({class_names[final_id]})")


def read_trigger_context(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            parts = f.readline().strip().split(",", 1)
        seq = int(parts[0]) if parts[0] else 0
        run_id = int(parts[1]) if len(parts) > 1 and parts[1] else 0
        return seq, run_id
    except (OSError, ValueError):
        return 0, 0


def write_result(path, algo_id, algo_type, result_code, seq):
    timestamp = int(time.time())
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"{algo_id},{algo_type},{result_code},{timestamp},{seq}\n")


def append_result_stream(path, run_id, algo_id, algo_type, result_code, seq):
    if not path:
        return

    timestamp = int(time.time())
    line = (
        f"{int(run_id)},{int(algo_id)},{algo_type},"
        f"{int(result_code)},{timestamp},{int(seq)}\n"
    ).encode("utf-8")
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
    try:
        written = os.write(fd, line)
        if written != len(line):
            raise OSError(f"short result-stream write: {written}/{len(line)} bytes")
    finally:
        os.close(fd)


def run_values(args, session, input_name, output_name, mean, scale, class_names, values):
    features = voltage_to_features24(values, args.input_type)
    pred_ids, probabilities, counts, final_id = infer_batch(
        session, input_name, output_name, features, mean, scale, class_names
    )
    print_result(pred_ids, probabilities, counts, final_id, class_names)
    return final_id


def csv_mode(args, session, input_name, output_name, mean, scale, class_names):
    values = read_csv_voltage(args.csv, args.has_header)
    print(f"CSV value count: {values.size}")
    print(f"Input type: {args.input_type}")
    print(f"feature samples: {values.size // RAW_POINTS}")
    run_values(args, session, input_name, output_name, mean, scale, class_names, values)


def once_mode(args, session, input_name, output_name, mean, scale, class_names):
    values = read_bin_voltage(
        args.data, args.max_ch, args.max_points, args.channel, args.offset, RAW_POINTS
    )
    result_code = run_values(
        args, session, input_name, output_name, mean, scale, class_names, values
    )
    write_result(args.result_file, args.algo_id, args.algo_type, result_code, 0)


def watch_mode(args, session, input_name, output_name, mean, scale, class_names):
    print(f"Watch: channel={args.channel}, voltage_count={RAW_POINTS}")
    print(f"Data: {args.data}")
    print(f"Trigger: {args.trigger}")
    print(f"Result: {args.result_file}")
    if args.result_stream_file:
        print(f"Result stream: {args.result_stream_file}")
    last_key = None

    while True:
        if os.path.exists(args.stop_file):
            print("stop file detected")
            return
        if os.path.exists(args.trigger):
            try:
                stat = os.stat(args.trigger)
                seq, run_id = read_trigger_context(args.trigger)
                key = (stat.st_mtime_ns, stat.st_size, seq, run_id)
            except OSError:
                key = None
            if key is not None and key != last_key:
                last_key = key
                try:
                    values = read_bin_voltage(
                        args.data,
                        args.max_ch,
                        args.max_points,
                        args.channel,
                        args.offset,
                        RAW_POINTS,
                    )
                    result_code = run_values(
                        args,
                        session,
                        input_name,
                        output_name,
                        mean,
                        scale,
                        class_names,
                        values,
                    )
                    write_result(
                        args.result_file,
                        args.algo_id,
                        args.algo_type,
                        result_code,
                        key[2],
                    )
                    append_result_stream(
                        args.result_stream_file,
                        key[3],
                        args.algo_id,
                        args.algo_type,
                        result_code,
                        key[2],
                    )
                except Exception as exc:
                    print(f"watch inference failed: {exc}")
        time.sleep(args.interval)


def realtime_mode(args, session, input_name, output_name, mean, scale, class_names):
    required = RAW_POINTS
    print(f"Enter {required} voltage values separated by spaces or commas; q exits.")
    while True:
        text = input("> ").strip()
        if text.lower() in {"q", "quit", "exit"}:
            return
        try:
            values = np.fromstring(text.replace(",", " "), sep=" ", dtype=np.float32)
            if values.size != required:
                raise ValueError(f"need {required} values, got {values.size}")
            run_values(args, session, input_name, output_name, mean, scale, class_names, values)
        except Exception as exc:
            print(f"input failed: {exc}")


def parse_args():
    parser = argparse.ArgumentParser(description="12-voltage to 24D four-class diagnosis")
    parser.add_argument("--mode", choices=["csv", "once", "watch", "realtime"], default="csv")
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--metadata", default=DEFAULT_METADATA)
    parser.add_argument("--scaler", default=DEFAULT_SCALER)
    parser.add_argument("--csv")
    parser.add_argument("--has-header", action="store_true")
    parser.add_argument(
        "--input-type",
        choices=["voltage", "beaker"],
        default="voltage",
        help="voltage decodes mu-law voltage to beaker_ml; beaker uses values without conversion",
    )
    parser.add_argument("--data", default="/tmp/algo_ly_zd_data.bin")
    parser.add_argument("--trigger", default="/tmp/algo_ly_zd_trigger.flag")
    parser.add_argument("--stop-file", default="/tmp/algo_ly_zd_stop.flag")
    parser.add_argument("--result-file", default="/tmp/algo_ly_zd_result.txt")
    parser.add_argument("--result-stream-file", default="")
    parser.add_argument("--algo-id", type=int, default=0)
    parser.add_argument("--algo-type", default="DIAG")
    parser.add_argument("--max-ch", type=int, default=8)
    parser.add_argument("--max-points", type=int, default=4096)
    parser.add_argument("--channel", type=int, default=3)
    parser.add_argument("--offset", type=int, default=0)
    parser.add_argument(
        "--vote-count",
        type=int,
        default=1,
        help="accepted for compatibility; C side controls voting, Python runs one sample",
    )
    parser.add_argument("--interval", type=float, default=0.05)
    args = parser.parse_args()
    if args.mode == "csv" and not args.csv:
        parser.error("--csv is required in csv mode")
    if args.vote_count <= 0:
        parser.error("--vote-count must be positive")
    return args


def main():
    args = parse_args()
    mean, scale, class_names = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_session(args.model)
    print(f"Classes: {class_names}")
    if args.mode == "csv":
        csv_mode(args, session, input_name, output_name, mean, scale, class_names)
    elif args.mode == "once":
        once_mode(args, session, input_name, output_name, mean, scale, class_names)
    elif args.mode == "watch":
        watch_mode(args, session, input_name, output_name, mean, scale, class_names)
    else:
        realtime_mode(args, session, input_name, output_name, mean, scale, class_names)


if __name__ == "__main__":
    main()
