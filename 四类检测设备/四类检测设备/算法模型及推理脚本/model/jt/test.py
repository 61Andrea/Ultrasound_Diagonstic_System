import os
import json
import csv
import argparse
import numpy as np
import onnxruntime as ort


# ============================================================
# 版本说明
# ============================================================
# 版本: infer1_notime_uart_metrics_v2
# 目标:
#   1. 串口/终端只显示最终准确率；CPU占用可选显示。
#   2. 逐条检测结果和完整评估指标保存到当前运行文件夹。
#   3. 默认数据标签规则: 前 normal_count 条为正常(0)，后面为异常(1)。


# ============================================================
# 1. 默认路径配置
# ============================================================

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MODEL_DIR = os.path.join(BASE_DIR, "model")
DEFAULT_DATA_PATH = os.path.join(BASE_DIR, "data", "jt.txt")


# ============================================================
# 2. 基础工具函数
# ============================================================

def check_file_exists(path, name):
    if not os.path.exists(path):
        raise FileNotFoundError("{} 不存在: {}".format(name, path))


def read_plain_feature_txt(file_path):
    """
    读取纯数字 txt。

    支持两种格式：
        1）一行 256 个数
        2）多行，每行 256 个数

    返回：
        np.float32 二维数组，shape = [样本数, 特征维度]
    """

    x = np.loadtxt(file_path, dtype=np.float32)

    if x.ndim == 1:
        x = x.reshape(1, -1)

    if x.ndim != 2:
        raise ValueError("输入数据必须是二维矩阵，当前维度: {}".format(x.shape))

    return x.astype(np.float32)


def load_json(path):
    check_file_exists(path, "JSON文件")
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def load_scaler(scaler_path):
    scaler_data = load_json(scaler_path)

    if "mean" not in scaler_data:
        raise KeyError("scaler.json 中没有 mean 字段")

    if "scale" in scaler_data:
        scale_key = "scale"
    elif "std" in scaler_data:
        scale_key = "std"
    else:
        raise KeyError("scaler.json 中没有 scale 或 std 字段")

    mean = np.array(scaler_data["mean"], dtype=np.float32).reshape(-1)
    scale = np.array(scaler_data[scale_key], dtype=np.float32).reshape(-1)
    scale = np.where(scale == 0, 1.0, scale).astype(np.float32)

    if mean.shape[0] != scale.shape[0]:
        raise ValueError(
            "scaler 维度不一致: mean={}, scale={}".format(
                mean.shape[0], scale.shape[0]
            )
        )

    return mean, scale


def load_threshold(meta_path):
    metadata = load_json(meta_path)

    threshold = metadata.get("threshold", None)
    if threshold is None:
        threshold = metadata.get("default_threshold", None)
    if threshold is None:
        threshold = metadata.get("mse_threshold", None)
    if threshold is None:
        threshold = metadata.get("reconstruction_threshold", None)

    if threshold is None:
        raise ValueError("metadata.json 里没有 threshold / default_threshold / mse_threshold")

    return float(threshold)


# ============================================================
# 3. CPU占用读取，不依赖 psutil
# ============================================================

def read_cpu_snapshot():
    """
    从 /proc/stat 读取系统CPU计数。

    返回：
        (idle_time, total_time)

    如果系统没有 /proc/stat，则返回 None。
    """

    try:
        with open("/proc/stat", "r") as f:
            line = f.readline().strip()
    except Exception:
        return None

    parts = line.split()
    if len(parts) < 5 or parts[0] != "cpu":
        return None

    values = []
    for item in parts[1:]:
        try:
            values.append(int(item))
        except ValueError:
            values.append(0)

    idle = values[3]
    if len(values) > 4:
        idle += values[4]  # iowait

    total = sum(values)
    return idle, total


def calc_cpu_usage_percent(before, after):
    """
    根据两次 /proc/stat 快照计算系统CPU占用百分比。
    """

    if before is None or after is None:
        return None

    idle_delta = after[0] - before[0]
    total_delta = after[1] - before[1]

    if total_delta <= 0:
        return None

    usage = (1.0 - float(idle_delta) / float(total_delta)) * 100.0

    if usage < 0:
        usage = 0.0
    if usage > 100:
        usage = 100.0

    return usage


# ============================================================
# 4. 指标计算
# ============================================================

def build_labels(total_count, normal_count):
    """
    生成真实标签：
        前 normal_count 条 = 正常 0
        后面所有样本 = 异常 1
    """

    if normal_count < 0:
        raise ValueError("normal_count 不能小于 0")

    if normal_count > total_count:
        raise ValueError(
            "normal_count({}) 大于样本总数({})".format(normal_count, total_count)
        )

    labels = np.ones(total_count, dtype=np.int32)
    labels[:normal_count] = 0
    return labels


def safe_div(num, den):
    if den == 0:
        return 0.0
    return float(num) / float(den)


def calculate_metrics(labels, preds):
    labels = np.array(labels, dtype=np.int32).reshape(-1)
    preds = np.array(preds, dtype=np.int32).reshape(-1)

    if labels.shape[0] != preds.shape[0]:
        raise ValueError(
            "标签数量和预测数量不一致: labels={}, preds={}".format(
                labels.shape[0], preds.shape[0]
            )
        )

    total = int(labels.shape[0])

    tn = int(np.sum((labels == 0) & (preds == 0)))
    fp = int(np.sum((labels == 0) & (preds == 1)))
    fn = int(np.sum((labels == 1) & (preds == 0)))
    tp = int(np.sum((labels == 1) & (preds == 1)))

    accuracy = safe_div(tp + tn, total)
    precision = safe_div(tp, tp + fp)
    recall = safe_div(tp, tp + fn)
    f1 = safe_div(2.0 * precision * recall, precision + recall)
    false_alarm_rate = safe_div(fp, fp + tn)
    miss_rate = safe_div(fn, fn + tp)

    return {
        "total": total,
        "label_normal_count": int(np.sum(labels == 0)),
        "label_abnormal_count": int(np.sum(labels == 1)),
        "pred_normal_count": int(np.sum(preds == 0)),
        "pred_abnormal_count": int(np.sum(preds == 1)),
        "tn": tn,
        "fp": fp,
        "fn": fn,
        "tp": tp,
        "accuracy": accuracy,
        "precision": precision,
        "recall": recall,
        "f1": f1,
        "false_alarm_rate": false_alarm_rate,
        "miss_rate": miss_rate,
        "accuracy_percent": accuracy * 100.0,
        "precision_percent": precision * 100.0,
        "recall_percent": recall * 100.0,
        "f1_percent": f1 * 100.0,
        "false_alarm_rate_percent": false_alarm_rate * 100.0,
        "miss_rate_percent": miss_rate * 100.0,
    }


# ============================================================
# 5. 保存结果到当前文件夹
# ============================================================

def save_detection_results(output_dir, errors, preds, labels, threshold):
    csv_path = os.path.join(output_dir, "detection_results.csv")
    txt_path = os.path.join(output_dir, "detection_results.txt")

    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "index",
            "error",
            "threshold",
            "prediction",
            "prediction_text",
            "label",
            "label_text",
            "correct",
        ])

        for i in range(len(preds)):
            pred = int(preds[i])
            label = int(labels[i])
            writer.writerow([
                i,
                "{:.10f}".format(float(errors[i])),
                "{:.10f}".format(float(threshold)),
                pred,
                "abnormal" if pred == 1 else "normal",
                label,
                "abnormal" if label == 1 else "normal",
                1 if pred == label else 0,
            ])

    with open(txt_path, "w", encoding="utf-8") as f:
        for i in range(len(preds)):
            pred = int(preds[i])
            label = int(labels[i])
            f.write(
                "第{}条: error={:.10f}, threshold={:.10f}, "
                "prediction={}, result={}, label={}, correct={}\n".format(
                    i,
                    float(errors[i]),
                    float(threshold),
                    pred,
                    "异常" if pred == 1 else "正常",
                    label,
                    1 if pred == label else 0,
                )
            )

    return csv_path, txt_path


def save_metrics(output_dir, metrics, cpu_usage_percent, config_info):
    metrics_json_path = os.path.join(output_dir, "metrics.json")
    metrics_txt_path = os.path.join(output_dir, "metrics.txt")

    data = dict(metrics)
    data["cpu_usage_percent"] = cpu_usage_percent
    data.update(config_info)

    with open(metrics_json_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)

    with open(metrics_txt_path, "w", encoding="utf-8") as f:
        f.write("========== 离线检测评估指标 ==========" + "\n")
        f.write("样本总数: {}\n".format(metrics["total"]))
        f.write("真实正常数量: {}\n".format(metrics["label_normal_count"]))
        f.write("真实异常数量: {}\n".format(metrics["label_abnormal_count"]))
        f.write("预测正常数量: {}\n".format(metrics["pred_normal_count"]))
        f.write("预测异常数量: {}\n".format(metrics["pred_abnormal_count"]))
        f.write("TN 正常判正常: {}\n".format(metrics["tn"]))
        f.write("FP 正常判异常: {}\n".format(metrics["fp"]))
        f.write("FN 异常判正常: {}\n".format(metrics["fn"]))
        f.write("TP 异常判异常: {}\n".format(metrics["tp"]))
        f.write("Accuracy 准确率: {:.2f}%\n".format(metrics["accuracy_percent"]))
        f.write("Precision 精确率: {:.2f}%\n".format(metrics["precision_percent"]))
        f.write("Recall 召回率: {:.2f}%\n".format(metrics["recall_percent"]))
        f.write("F1: {:.2f}%\n".format(metrics["f1_percent"]))
        f.write("误报率 False Alarm Rate: {:.2f}%\n".format(metrics["false_alarm_rate_percent"]))
        f.write("漏报率 Miss Rate: {:.2f}%\n".format(metrics["miss_rate_percent"]))

        if cpu_usage_percent is None:
            f.write("CPU占用: N/A\n")
        else:
            f.write("CPU占用: {:.2f}%\n".format(cpu_usage_percent))

        f.write("\n========== 配置 ==========" + "\n")
        for key in sorted(config_info.keys()):
            f.write("{}: {}\n".format(key, config_info[key]))

    return metrics_json_path, metrics_txt_path


# ============================================================
# 6. 主流程
# ============================================================

def parse_args():
    parser = argparse.ArgumentParser(
        description="ONNX自编码器离线检测：串口仅显示准确率/可选CPU占用，完整结果保存到当前文件夹"
    )

    parser.add_argument(
        "--model-dir",
        type=str,
        default=DEFAULT_MODEL_DIR,
        help="模型文件夹，默认是脚本同级 model 文件夹"
    )

    parser.add_argument(
        "--data",
        type=str,
        default=DEFAULT_DATA_PATH,
        help="待检测 txt 文件路径，默认是脚本同级 data/jt.txt"
    )

    parser.add_argument(
        "--normal-count",
        type=int,
        default=200,
        help="前多少条样本作为正常标签0，默认200；后面自动作为异常标签1"
    )

    parser.add_argument(
        "--expected-dim",
        type=int,
        default=256,
        help="期望输入特征维度，默认256"
    )

    parser.add_argument(
        "--no-cpu",
        action="store_true",
        help="关闭串口/终端CPU占用显示；准确率仍然显示"
    )

    parser.add_argument(
        "--output-dir",
        type=str,
        default=os.getcwd(),
        help="结果保存目录，默认是当前运行文件夹"
    )

    return parser.parse_args()


def main():
    args = parse_args()

    output_dir = os.path.abspath(args.output_dir)
    os.makedirs(output_dir, exist_ok=True)

    model_dir = os.path.abspath(args.model_dir)
    onnx_path = os.path.join(model_dir, "sae_model.onnx")
    scaler_path = os.path.join(model_dir, "scaler.json")
    meta_path = os.path.join(model_dir, "metadata.json")
    data_path = os.path.abspath(args.data)

    check_file_exists(onnx_path, "ONNX模型文件")
    check_file_exists(scaler_path, "scaler.json")
    check_file_exists(meta_path, "metadata.json")
    check_file_exists(data_path, "数据文件")

    cpu_before = None
    if not args.no_cpu:
        cpu_before = read_cpu_snapshot()

    mean, scale = load_scaler(scaler_path)
    threshold = load_threshold(meta_path)

    session = ort.InferenceSession(
        onnx_path,
        providers=["CPUExecutionProvider"]
    )

    input_name = session.get_inputs()[0].name
    output_name = session.get_outputs()[0].name

    x = read_plain_feature_txt(data_path)

    if x.shape[1] != mean.shape[0]:
        raise ValueError(
            "输入特征维度不对，当前是 {} 维，但 scaler 需要 {} 维".format(
                x.shape[1], mean.shape[0]
            )
        )

    if x.shape[1] != args.expected_dim:
        raise ValueError(
            "模型要求输入 {} 维，当前输入是 {} 维".format(
                args.expected_dim, x.shape[1]
            )
        )

    labels = build_labels(total_count=x.shape[0], normal_count=args.normal_count)

    x_scaled = (x - mean) / scale
    x_scaled = x_scaled.astype(np.float32)

    y_recon = session.run(
        [output_name],
        {input_name: x_scaled}
    )[0]

    errors = np.mean(np.square(x_scaled - y_recon), axis=1)
    preds = (errors > threshold).astype(np.int32)

    metrics = calculate_metrics(labels, preds)

    cpu_usage_percent = None
    if not args.no_cpu:
        cpu_after = read_cpu_snapshot()
        cpu_usage_percent = calc_cpu_usage_percent(cpu_before, cpu_after)

    detection_csv_path, detection_txt_path = save_detection_results(
        output_dir=output_dir,
        errors=errors,
        preds=preds,
        labels=labels,
        threshold=threshold
    )

    config_info = {
        "model_dir": model_dir,
        "onnx_path": onnx_path,
        "scaler_path": scaler_path,
        "metadata_path": meta_path,
        "data_path": data_path,
        "output_dir": output_dir,
        "threshold": threshold,
        "expected_dim": args.expected_dim,
        "normal_count": args.normal_count,
        "abnormal_count": int(x.shape[0] - args.normal_count),
        "detection_results_csv": detection_csv_path,
        "detection_results_txt": detection_txt_path,
    }

    save_metrics(
        output_dir=output_dir,
        metrics=metrics,
        cpu_usage_percent=cpu_usage_percent,
        config_info=config_info
    )

    # ========================================================
    # 串口/终端最终只显示这里
    # ========================================================
    print("准确率: {:.2f}%".format(metrics["accuracy_percent"]))

    if not args.no_cpu:
        if cpu_usage_percent is None:
            print("CPU占用: N/A")
        else:
            print("CPU占用: {:.2f}%".format(cpu_usage_percent))


if __name__ == "__main__":
    main()
