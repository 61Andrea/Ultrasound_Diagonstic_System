import os
import json
import time
import argparse
import numpy as np
import onnxruntime as ort


# =========================
# 默认文件路径
# =========================

MODEL_PATH = "/home/root/model/bearing_jc/model/sae_model.onnx"
METADATA_PATH = "/home/root/model/bearing_jc/model/metadata.json"
SCALER_PATH = "/home/root/model/bearing_jc/model/scaler.json"


# =========================

# 基础工具函数
# =========================

def load_json(path):
    if not os.path.exists(path):
        raise FileNotFoundError(f"文件不存在: {path}")

    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def get_value_from_dict(d, possible_keys, default=None):
    for key in possible_keys:
        if key in d:
            return d[key]
    return default


def to_2d_float32(arr):
    arr = np.array(arr, dtype=np.float32)

    if arr.ndim == 1:
        arr = arr.reshape(1, -1)

    if arr.ndim != 2:
        raise ValueError(f"数组维度错误，期望二维数组，实际 shape={arr.shape}")

    return arr


# =========================
# 加载 metadata 和 scaler
# =========================

def load_config(metadata_path, scaler_path):
    """
    加载 metadata.json 和 scaler.json。

    支持以下几种常见字段名：

    metadata.json:
        input_dim / feature_dim / n_features / dim
        threshold / mse_threshold / reconstruction_threshold

    scaler.json:
        mean / mean_ / scaler_mean
        std / scale_ / scaler_std
    """

    meta = load_json(metadata_path)

    if os.path.exists(scaler_path):
        scaler = load_json(scaler_path)
    else:
        print(f"警告: 未找到 {scaler_path}，尝试从 metadata.json 中读取 mean/std")
        scaler = {}

    input_dim = get_value_from_dict(
        meta,
        ["input_dim", "feature_dim", "n_features", "dim"],
        default=None
    )

    threshold = get_value_from_dict(
        meta,
        ["threshold", "mse_threshold", "reconstruction_threshold"],
        default=None
    )

    mean = get_value_from_dict(
        scaler,
        ["mean", "mean_", "scaler_mean"],
        default=None
    )

    std = get_value_from_dict(
        scaler,
        ["std", "scale_", "scaler_std"],
        default=None
    )

    if mean is None:
        mean = get_value_from_dict(
            meta,
            ["mean", "mean_", "scaler_mean"],
            default=None
        )

    if std is None:
        std = get_value_from_dict(
            meta,
            ["std", "scale_", "scaler_std"],
            default=None
        )

    if mean is not None:
        mean = np.array(mean, dtype=np.float32).reshape(1, -1)

    if std is not None:
        std = np.array(std, dtype=np.float32).reshape(1, -1)

    if input_dim is None:
        if mean is not None:
            input_dim = mean.shape[1]
        else:
            raise ValueError("无法确定 input_dim，请检查 metadata.json 或 scaler.json")

    input_dim = int(input_dim)

    if threshold is None:
        raise ValueError("没有找到 threshold，请检查 metadata.json")

    threshold = float(threshold)

    if mean is None:
        print("警告: 未找到 mean，默认使用全 0")
        mean = np.zeros((1, input_dim), dtype=np.float32)

    if std is None:
        print("警告: 未找到 std/scale_，默认使用全 1")
        std = np.ones((1, input_dim), dtype=np.float32)

    if mean.shape[1] != input_dim:
        raise ValueError(f"mean 维度不匹配: mean.shape={mean.shape}, input_dim={input_dim}")

    if std.shape[1] != input_dim:
        raise ValueError(f"std 维度不匹配: std.shape={std.shape}, input_dim={input_dim}")

    std = np.where(std == 0, 1.0, std).astype(np.float32)

    return input_dim, threshold, mean, std


# =========================
# ONNX 模型加载
# =========================

def create_onnx_session(model_path):
    if not os.path.exists(model_path):
        raise FileNotFoundError(f"模型文件不存在: {model_path}")

    session = ort.InferenceSession(
        model_path,
        providers=["CPUExecutionProvider"]
    )

    input_name = session.get_inputs()[0].name
    output_name = session.get_outputs()[0].name

    input_shape = session.get_inputs()[0].shape
    output_shape = session.get_outputs()[0].shape

    print("ONNX 模型加载成功")
    print("模型路径:", model_path)
    print("输入名:", input_name)
    print("输入 shape:", input_shape)
    print("输出名:", output_name)
    print("输出 shape:", output_shape)

    return session, input_name, output_name


# =========================
# 数据处理与推理
# =========================

def normalize(x_raw, mean, std):
    return (x_raw - mean) / std


def infer_once(session, input_name, output_name, x_raw, mean, std, threshold):
    """
    单次检测：
        原始数据
        -> 标准化
        -> ONNX 推理
        -> 计算重构误差 MSE
        -> 阈值判断
    """

    x_raw = to_2d_float32(x_raw)

    if x_raw.shape[1] != mean.shape[1]:
        raise ValueError(
            f"输入维度错误: x_raw.shape={x_raw.shape}, 模型需要 {mean.shape[1]} 维"
        )

    x_norm = normalize(x_raw, mean, std).astype(np.float32)

    y = session.run([output_name], {input_name: x_norm})[0]
    y = to_2d_float32(y)

    if y.shape != x_norm.shape:
        raise ValueError(
            f"模型输出维度和输入维度不一致: input={x_norm.shape}, output={y.shape}"
        )

    mse = np.mean((x_norm - y) ** 2)

    is_fault = mse > threshold

    return {
        "mse": float(mse),
        "threshold": float(threshold),
        "is_fault": bool(is_fault),
        "x_raw": x_raw,
        "x_norm": x_norm,
        "y": y
    }


# =========================
# 随机数据检测
# =========================

def get_random_data(input_dim, mean, std):
    """
    生成一组模拟原始数据。

    这里先生成标准正态随机数，再反标准化到原始数据范围附近：
        x_raw = random_norm * std + mean

    这样生成的数据一般接近训练数据分布。
    """

    random_norm = np.random.randn(1, input_dim).astype(np.float32)
    x_raw = random_norm * std + mean
    return x_raw.astype(np.float32)


def random_test(args):
    input_dim, threshold, mean, std = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)

    print("输入维度:", input_dim)
    print("阈值:", threshold)
    print("开始随机数据检测")
    print("==============================")

    normal_count = 0
    fault_count = 0

    for i in range(args.loop):
        x_raw = get_random_data(input_dim, mean, std)

        result = infer_once(
            session=session,
            input_name=input_name,
            output_name=output_name,
            x_raw=x_raw,
            mean=mean,
            std=std,
            threshold=threshold
        )

        if result["is_fault"]:
            fault_count += 1
            state = "FAULT"
        else:
            normal_count += 1
            state = "NORMAL"

        print(
            f"第 {i + 1} 次: "
            f"MSE={result['mse']:.6f}, "
            f"threshold={result['threshold']:.6f}, "
            f"result={state}"
        )

        if args.print_data:
            print("原始输入前10维:", result["x_raw"][0, :10])
            print("标准化输入前10维:", result["x_norm"][0, :10])
            print("模型输出前10维:", result["y"][0, :10])

        time.sleep(args.interval)

    print("==============================")
    print("随机检测完成")
    print("正常数量:", normal_count)
    print("异常数量:", fault_count)


# =========================
# CSV 数据检测
# =========================

def read_csv_data(csv_path, input_dim, has_header=False, has_label=False):
    if not os.path.exists(csv_path):
        raise FileNotFoundError(f"CSV 文件不存在: {csv_path}")

    skiprows = 1 if has_header else 0

    data = np.loadtxt(
        csv_path,
        delimiter=",",
        skiprows=skiprows,
        dtype=np.float32
    )

    data = to_2d_float32(data)

    labels = None

    if has_label:
        if data.shape[1] < 2:
            raise ValueError("CSV 设置了 has_label，但列数小于 2")
        labels = data[:, -1]
        data = data[:, :-1]

    if data.shape[1] != input_dim:
        raise ValueError(
            f"CSV 特征维度不匹配: CSV特征列数={data.shape[1]}, 模型需要={input_dim}。"
            f"如果CSV最后一列是标签，请加参数 --has-label。"
            f"如果CSV第一行是表头，请加参数 --has-header。"
        )

    return data.astype(np.float32), labels


def csv_test(args):
    input_dim, threshold, mean, std = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)

    data, labels = read_csv_data(
        csv_path=args.csv,
        input_dim=input_dim,
        has_header=args.has_header,
        has_label=args.has_label
    )

    print("输入维度:", input_dim)
    print("阈值:", threshold)
    print("CSV路径:", args.csv)
    print("CSV样本数量:", data.shape[0])
    print("开始 CSV 检测")
    print("==============================")

    normal_count = 0
    fault_count = 0
    correct_count = 0
    has_label = labels is not None

    for i in range(data.shape[0]):
        x_raw = data[i].reshape(1, input_dim)

        result = infer_once(
            session=session,
            input_name=input_name,
            output_name=output_name,
            x_raw=x_raw,
            mean=mean,
            std=std,
            threshold=threshold
        )

        if result["is_fault"]:
            fault_count += 1
            pred = 1
            state = "FAULT"
        else:
            normal_count += 1
            pred = 0
            state = "NORMAL"

        line = (
            f"第 {i + 1} 行: "
            f"MSE={result['mse']:.6f}, "
            f"threshold={result['threshold']:.6f}, "
            f"result={state}"
        )

        if has_label:
            label = int(labels[i])
            if pred == label:
                correct_count += 1
            line += f", label={label}"

        print(line)

        if args.print_data:
            print("原始输入前10维:", result["x_raw"][0, :10])
            print("标准化输入前10维:", result["x_norm"][0, :10])
            print("模型输出前10维:", result["y"][0, :10])

    print("==============================")
    print("CSV 检测完成")
    print("正常数量:", normal_count)
    print("异常数量:", fault_count)

    if has_label:
        acc = correct_count / data.shape[0]
        print("标签正确数量:", correct_count)
        print("准确率:", f"{acc * 100:.2f}%")


# =========================
# 实时数据接口
# =========================

def get_realtime_data(input_dim):
    """
    预留真实数据接口。

    后面你接自己的数据时，只改这个函数。

    要求返回：
        shape = [1, input_dim]
        dtype = float32

    示例1：你的采集函数返回 list
        data = read_from_dma()
        return np.array(data, dtype=np.float32).reshape(1, input_dim)

    示例2：你的数据存在某个 txt/csv 临时文件
        data = np.loadtxt("/tmp/realtime_data.txt", dtype=np.float32)
        return data.reshape(1, input_dim)
    """

    # TODO: 在这里接你的真实采集数据
    raise NotImplementedError("请在 get_realtime_data() 中接入你的真实数据")


def realtime_test(args):
    input_dim, threshold, mean, std = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)

    print("输入维度:", input_dim)
    print("阈值:", threshold)
    print("开始实时检测")
    print("==============================")

    index = 0

    while True:
        index += 1

        x_raw = get_realtime_data(input_dim)

        result = infer_once(
            session=session,
            input_name=input_name,
            output_name=output_name,
            x_raw=x_raw,
            mean=mean,
            std=std,
            threshold=threshold
        )

        if result["is_fault"]:
            state = "FAULT"
        else:
            state = "NORMAL"

        print(
            f"第 {index} 次: "
            f"MSE={result['mse']:.6f}, "
            f"threshold={result['threshold']:.6f}, "
            f"result={state}"
        )

        time.sleep(args.interval)


# =========================
# 主函数
# =========================

def parse_args():
    parser = argparse.ArgumentParser(description="ONNX 稀疏自编码器故障检测程序")

    parser.add_argument(
        "--mode",
        type=str,
        default="random",
        choices=["random", "csv", "realtime"],
        help="检测模式: random / csv / realtime"
    )

    parser.add_argument(
        "--model",
        type=str,
        default=MODEL_PATH,
        help="ONNX模型路径"
    )

    parser.add_argument(
        "--metadata",
        type=str,
        default=METADATA_PATH,
        help="metadata.json路径"
    )

    parser.add_argument(
        "--scaler",
        type=str,
        default=SCALER_PATH,
        help="scaler.json路径"
    )

    parser.add_argument(
        "--csv",
        type=str,
        default="/home/root/model/test_data.csv",
        help="CSV测试数据路径"
    )

    parser.add_argument(
        "--has-header",
        action="store_true",
        help="CSV第一行是否为表头"
    )

    parser.add_argument(
        "--has-label",
        action="store_true",
        help="CSV最后一列是否为标签，0表示正常，1表示故障"
    )

    parser.add_argument(
        "--loop",
        type=int,
        default=10,
        help="随机测试次数"
    )

    parser.add_argument(
        "--interval",
        type=float,
        default=1.0,
        help="检测间隔，单位秒"
    )

    parser.add_argument(
        "--print-data",
        action="store_true",
        help="是否打印输入和输出的前10维数据"
    )

    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()

    if args.mode == "random":
        random_test(args)

    elif args.mode == "csv":
        csv_test(args)

    elif args.mode == "realtime":
        realtime_test(args)