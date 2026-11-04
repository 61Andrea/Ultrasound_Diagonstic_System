import os
import json
import time
import argparse
import numpy as np
import onnxruntime as ort


# =========================
# 默认路径
# =========================

DEFAULT_MODEL_PATH = "/home/root/model/zz_jc/model/sae_model.onnx"
DEFAULT_METADATA_PATH = "/home/root/model/zz_jc/model/metadata.json"
DEFAULT_SCALER_PATH = "/home/root/model/zz_jc/model/scaler.json"
DEFAULT_CSV_PATH = "/home/root/model/zz_jc/data/data.csv"


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
# 加载配置
# =========================

def load_config(metadata_path, scaler_path):
    """
    加载 metadata.json 和 scaler.json。

    支持这些字段名：

    输入维度：
        input_dim / win_size / seq_len / feature_dim / n_features / dim

    阈值：
        threshold / mse_threshold / reconstruction_threshold

    均值：
        mean / mean_ / scaler_mean

    标准差：
        std / std_ / scale / scale_ / scaler_std

    你的 scaler.json 里是：
        mean
        scale
    """

    meta = load_json(metadata_path)

    if os.path.exists(scaler_path):
        scaler = load_json(scaler_path)
    else:
        print(f"警告: 未找到 {scaler_path}，尝试从 metadata.json 中读取 mean/std/scale")
        scaler = {}

    input_dim = get_value_from_dict(
        meta,
        ["input_dim", "win_size", "seq_len", "feature_dim", "n_features", "dim"],
        default=None
    )

    threshold = get_value_from_dict(
        meta,
        ["threshold", "mse_threshold", "reconstruction_threshold"],
        default=None
    )

    hop = get_value_from_dict(
        meta,
        ["hop", "stride"],
        default=128
    )

    mean = get_value_from_dict(
        scaler,
        ["mean", "mean_", "scaler_mean"],
        default=None
    )

    scale = get_value_from_dict(
        scaler,
        ["std", "std_", "scale", "scale_", "scaler_std"],
        default=None
    )

    if mean is None:
        mean = get_value_from_dict(
            meta,
            ["mean", "mean_", "scaler_mean"],
            default=None
        )

    if scale is None:
        scale = get_value_from_dict(
            meta,
            ["std", "std_", "scale", "scale_", "scaler_std"],
            default=None
        )

    if mean is not None:
        mean = np.array(mean, dtype=np.float32).reshape(1, -1)

    if scale is not None:
        scale = np.array(scale, dtype=np.float32).reshape(1, -1)

    if input_dim is None:
        if mean is not None:
            input_dim = mean.shape[1]
        elif scale is not None:
            input_dim = scale.shape[1]
        else:
            raise ValueError("无法确定 input_dim，请检查 metadata.json 或 scaler.json")

    input_dim = int(input_dim)
    hop = int(hop)

    if threshold is None:
        raise ValueError("没有找到 threshold，请检查 metadata.json")

    threshold = float(threshold)

    if mean is None:
        print("警告: 未找到 mean，默认使用全 0")
        mean = np.zeros((1, input_dim), dtype=np.float32)

    if scale is None:
        print("警告: 未找到 std/scale/scale_，默认使用全 1")
        scale = np.ones((1, input_dim), dtype=np.float32)

    if mean.shape[1] != input_dim:
        raise ValueError(
            f"mean 维度不匹配: mean.shape={mean.shape}, input_dim={input_dim}"
        )

    if scale.shape[1] != input_dim:
        raise ValueError(
            f"scale 维度不匹配: scale.shape={scale.shape}, input_dim={input_dim}"
        )

    scale = np.where(scale == 0, 1.0, scale).astype(np.float32)

    print("配置加载成功")
    print("input_dim:", input_dim)
    print("hop:", hop)
    print("threshold:", threshold)
    print("mean shape:", mean.shape)
    print("scale shape:", scale.shape)

    return input_dim, hop, threshold, mean, scale


# =========================
# ONNX 模型加载
# =========================

def create_onnx_session(model_path):
    if not os.path.exists(model_path):
        raise FileNotFoundError(f"ONNX模型不存在: {model_path}")

    session = ort.InferenceSession(
        model_path,
        providers=["CPUExecutionProvider"]
    )

    input_info = session.get_inputs()[0]
    output_info = session.get_outputs()[0]

    input_name = input_info.name
    output_name = output_info.name

    print("ONNX模型加载成功")
    print("模型路径:", model_path)
    print("输入名:", input_name)
    print("输入shape:", input_info.shape)
    print("输出名:", output_name)
    print("输出shape:", output_info.shape)

    return session, input_name, output_name


# =========================
# 二进制数据读取
# =========================

def read_bin_window(bin_path, seq_len, max_ch=8, max_points=4096, channel=3, offset=0):
    """
    从 C 语言写出的二进制文件读取指定通道的窗口数据。

    C 端数据格式: int new_data[MAX_CH][MAX_POINTS]
    写入方式: fwrite(new_data, sizeof(int), MAX_CH * MAX_POINTS, fp)

    参数:
        bin_path:   二进制文件路径，如 /tmp/algo_data.bin
        seq_len:    窗口长度，等于模型的 input_dim
        max_ch:     最大通道数，默认 8
        max_points: 每通道最大点数，默认 4096
        channel:    要读取的通道号，默认 3
        offset:     窗口起始偏移，默认 0

    返回:
        np.float32 一维数组，长度为 seq_len
    """

    raw = np.fromfile(bin_path, dtype=np.int32)

    expected_size = max_ch * max_points
    if raw.size != expected_size:
        raise ValueError(
            f"二进制文件大小不匹配: 期望 {expected_size} 个int32, 实际 {raw.size}"
        )

    data = raw.reshape(max_ch, max_points)

    if channel < 0 or channel >= max_ch:
        raise ValueError(f"channel={channel} 超出范围 [0, {max_ch - 1}]")

    if offset + seq_len > max_points:
        raise ValueError(
            f"offset({offset}) + seq_len({seq_len}) = {offset + seq_len} "
            f"超出 max_points({max_points})"
        )

    window = data[channel, offset:offset + seq_len]

    return window.astype(np.float32)


# =========================
# 标准化与推理
# =========================

def normalize(x_raw, mean, scale):
    """
    标准化：
        x_norm = (x_raw - mean) / scale

    你的 scaler.json 里字段叫 scale。
    """

    return (x_raw - mean) / scale


def infer_once(session, input_name, output_name, x_raw, mean, scale, threshold):
    """
    单个512维窗口检测：

    原始窗口
    -> 标准化
    -> ONNX自编码器重构
    -> 计算 raw_mse
    -> 和 threshold 比较
    """

    x_raw = to_2d_float32(x_raw)

    if x_raw.shape[1] != mean.shape[1]:
        raise ValueError(
            f"输入维度错误: 当前输入 shape={x_raw.shape}, 模型需要 {mean.shape[1]} 维"
        )

    x_norm = normalize(x_raw, mean, scale).astype(np.float32)

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
# 随机测试
# =========================

def get_random_data(input_dim, mean, scale):
    """
    生成一组接近训练分布的随机原始数据。

    先生成标准正态随机数 random_norm，
    再反标准化：
        x_raw = random_norm * scale + mean
    """

    random_norm = np.random.randn(1, input_dim).astype(np.float32)
    x_raw = random_norm * scale + mean
    return x_raw.astype(np.float32)


def random_test(args):
    input_dim, hop, threshold, mean, scale = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)

    print("==============================")
    print("开始随机数据检测")
    print("==============================")

    normal_count = 0
    fault_count = 0

    for i in range(args.loop):
        x_raw = get_random_data(input_dim, mean, scale)

        result = infer_once(
            session=session,
            input_name=input_name,
            output_name=output_name,
            x_raw=x_raw,
            mean=mean,
            scale=scale,
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
            f"MSE={result['mse']:.8f}, "
            f"threshold={result['threshold']:.8f}, "
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
# CSV 读取与切窗
# =========================

def read_csv_series(csv_path, has_header=False, column=0):
    """
    读取CSV时间序列。

    支持：
    1. CSV只有一列
    2. CSV有多列，使用 --column 选择某一列

    column从0开始：
        0 = 第一列
        1 = 第二列
        2 = 第三列
    """

    if not os.path.exists(csv_path):
        raise FileNotFoundError(f"CSV文件不存在: {csv_path}")

    skiprows = 1 if has_header else 0

    data = np.loadtxt(
        csv_path,
        delimiter=",",
        skiprows=skiprows,
        dtype=np.float32
    )

    data = np.array(data, dtype=np.float32)

    if data.ndim == 1:
        series = data
    elif data.ndim == 2:
        if column < 0 or column >= data.shape[1]:
            raise ValueError(
                f"column={column} 超出范围，CSV共有 {data.shape[1]} 列"
            )
        series = data[:, column]
    else:
        raise ValueError(f"CSV数据维度异常: shape={data.shape}")

    series = np.array(series, dtype=np.float32).reshape(-1)

    return series


def read_csv_windows(csv_path, input_dim, has_header=False, has_label=False):
    """
    读取已经切好窗口的CSV。

    适合这种格式：
        每一行 = 一个512维窗口

    如果最后一列是标签，可以加 --has-label。
    """

    if not os.path.exists(csv_path):
        raise FileNotFoundError(f"CSV文件不存在: {csv_path}")

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
        labels = data[:, -1]
        data = data[:, :-1]

    if data.shape[1] != input_dim:
        raise ValueError(
            f"CSV窗口维度不匹配: CSV特征列数={data.shape[1]}, 模型需要={input_dim}。"
            f"如果最后一列是标签，请加 --has-label。"
        )

    return data.astype(np.float32), labels


def split_windows(series, win_size, hop):
    """
    把一条长时间序列切成多个窗口。

    例如：
        win_size = 512
        hop = 128

    窗口：
        0: [0, 512)
        1: [128, 640)
        2: [256, 768)
    """

    series = np.array(series, dtype=np.float32).reshape(-1)

    if series.shape[0] < win_size:
        raise ValueError(
            f"CSV数据长度不足: 当前长度={series.shape[0]}, 至少需要={win_size}"
        )

    windows = []
    starts = []

    for start in range(0, series.shape[0] - win_size + 1, hop):
        end = start + win_size
        windows.append(series[start:end])
        starts.append(start)

    windows = np.array(windows, dtype=np.float32)

    return windows, starts


def csv_test(args):
    input_dim, hop, threshold, mean, scale = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)

    if args.csv_format == "series":
        series = read_csv_series(
            csv_path=args.csv,
            has_header=args.has_header,
            column=args.column
        )

        data, starts = split_windows(
            series=series,
            win_size=input_dim,
            hop=hop
        )

        labels = None

        print("==============================")
        print("CSV格式: 长时间序列，程序自动切窗")
        print("CSV路径:", args.csv)
        print("读取列 column:", args.column)
        print("序列长度:", series.shape[0])
        print("窗口长度:", input_dim)
        print("滑动步长:", hop)
        print("窗口数量:", data.shape[0])
        print("==============================")

    elif args.csv_format == "windows":
        data, labels = read_csv_windows(
            csv_path=args.csv,
            input_dim=input_dim,
            has_header=args.has_header,
            has_label=args.has_label
        )

        starts = list(range(data.shape[0]))

        print("==============================")
        print("CSV格式: 每行一个窗口")
        print("CSV路径:", args.csv)
        print("窗口数量:", data.shape[0])
        print("窗口维度:", input_dim)
        print("==============================")

    else:
        raise ValueError("csv_format 只能是 series 或 windows")

    normal_count = 0
    fault_count = 0
    correct_count = 0
    has_label = labels is not None

    print("开始CSV检测")

    for i in range(data.shape[0]):
        x_raw = data[i].reshape(1, input_dim)

        result = infer_once(
            session=session,
            input_name=input_name,
            output_name=output_name,
            x_raw=x_raw,
            mean=mean,
            scale=scale,
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
            f"窗口 {i + 1:04d}, "
            f"start={starts[i]}, "
            f"MSE={result['mse']:.8f}, "
            f"threshold={result['threshold']:.8f}, "
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
    print("CSV检测完成")
    print("正常窗口数量:", normal_count)
    print("异常窗口数量:", fault_count)

    total = normal_count + fault_count
    if total > 0:
        print("正常比例:", f"{normal_count / total * 100:.2f}%")
        print("异常比例:", f"{fault_count / total * 100:.2f}%")

    if has_label:
        acc = correct_count / data.shape[0]
        print("标签正确数量:", correct_count)
        print("准确率:", f"{acc * 100:.2f}%")

    if fault_count > normal_count:
        print("整条数据最终判断: FAULT 异常")
    else:
        print("整条数据最终判断: NORMAL 正常")


# =========================
# 实时数据接口
# =========================

def get_realtime_window(input_dim):
    """
    预留实时采集接口。

    后面你接自己的数据时，只需要改这里。

    要求返回：
        一维数组，长度 input_dim，也就是512个点。

    示例：
        data = read_from_dma()
        return np.array(data, dtype=np.float32).reshape(input_dim)

    注意：
        这里返回原始数据。
        程序会自动做：
            x_norm = (x_raw - mean) / scale
    """

    raise NotImplementedError("请在 get_realtime_window() 中接入你的真实采集数据")


def realtime_test(args):
    input_dim, hop, threshold, mean, scale = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)

    print("==============================")
    print("开始实时检测")
    print("窗口长度:", input_dim)
    print("阈值:", threshold)
    print("==============================")

    index = 0

    while True:
        index += 1

        x_raw = get_realtime_window(input_dim)
        x_raw = np.array(x_raw, dtype=np.float32).reshape(1, input_dim)

        result = infer_once(
            session=session,
            input_name=input_name,
            output_name=output_name,
            x_raw=x_raw,
            mean=mean,
            scale=scale,
            threshold=threshold
        )

        if result["is_fault"]:
            state = "FAULT"
        else:
            state = "NORMAL"

        print(
            f"第 {index} 次实时检测: "
            f"MSE={result['mse']:.8f}, "
            f"threshold={result['threshold']:.8f}, "
            f"result={state}"
        )

        time.sleep(args.interval)




def read_trigger_seq(trigger_path):
    """读取 C 端写入 trigger 文件中的递增序号 seq。"""
    try:
        with open(trigger_path, "r", encoding="utf-8") as f:
            first = f.readline().strip()
        return int(first) if first else 0
    except Exception:
        return 0


def write_watch_result(result_file, algo_id, algo_type, result_code, seq):
    """统一结果格式: algo_id,algo_type,result_code,timestamp,seq"""
    timestamp = int(time.time())
    tmp_file = result_file + ".tmp"
    with open(tmp_file, "w", encoding="utf-8") as f:
        f.write(f"{int(algo_id)},{algo_type},{int(result_code)},{timestamp},{int(seq)}\n")
    os.replace(tmp_file, result_file)

# =========================
# 连续检测 watch 模式
# =========================

def watch_test(args):
    """
    Python 常驻进程，连续监听 C 端写出的数据文件并完成推理。

    工作流程:
        1. 启动时加载配置和 ONNX 模型（只加载一次）。
        2. 循环监听 trigger 文件（默认 /tmp/algo_trigger.flag）。
        3. 当 trigger 文件的修改时间变化时，读取 data 文件（默认 /tmp/algo_data.bin）。
        4. 使用 read_bin_window() 读取窗口，调用 infer_once() 推理。
        5. 输出检测结果（MSE + NORMAL/FAULT），写入 /tmp/algo_last_result.txt。
        6. 如果检测到 stop_file（默认 /tmp/algo_stop.flag），退出循环。
    """

    input_dim, hop, threshold, mean, scale = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)

    print("========== 连续检测 watch 模式 ==========")
    print("数据文件:", args.data)
    print("触发文件:", args.trigger)
    print("停止文件:", args.stop_file)
    print("结果文件:", args.result_file)
    print("algo_id:", args.algo_id)
    print("algo_type:", args.algo_type)
    print("max_ch:", args.max_ch)
    print("max_points:", args.max_points)
    print("channel:", args.channel)
    print("offset:", args.offset)
    print("input_dim:", input_dim)
    print("threshold:", threshold)
    print("扫描间隔:", args.interval, "秒")
    print("========================================")

    last_mtime = 0
    index = 0

    while True:
        # 检查停止文件
        if os.path.exists(args.stop_file):
            print("[watch] 检测到停止文件，退出连续检测模式")
            break

        # 检查触发文件
        if os.path.exists(args.trigger):
            try:
                current_mtime = os.path.getmtime(args.trigger)
            except OSError:
                current_mtime = 0

            if current_mtime != last_mtime:
                last_mtime = current_mtime

                # 读取数据窗口
                try:
                    window = read_bin_window(
                        bin_path=args.data,
                        seq_len=input_dim,
                        max_ch=args.max_ch,
                        max_points=args.max_points,
                        channel=args.channel,
                        offset=args.offset
                    )
                except Exception as e:
                    print(f"[watch] 读取数据失败: {e}")
                    time.sleep(args.interval)
                    continue

                # 推理
                result = infer_once(
                    session=session,
                    input_name=input_name,
                    output_name=output_name,
                    x_raw=window,
                    mean=mean,
                    scale=scale,
                    threshold=threshold
                )

                index += 1

                if result["is_fault"]:
                    state = "FAULT"
                else:
                    state = "NORMAL"

                print(
                    f"[watch #{index}] "
                    f"MSE={result['mse']:.8f}, "
                    f"threshold={result['threshold']:.8f}, "
                    f"result={state}"
                )

                # 写入统一结果文件: algo_id,algo_type,result_code,timestamp,seq
                try:
                    seq = read_trigger_seq(args.trigger)
                    result_code = 1 if result["is_fault"] else 0
                    write_watch_result(
                        result_file=args.result_file,
                        algo_id=args.algo_id,
                        algo_type=args.algo_type,
                        result_code=result_code,
                        seq=seq
                    )
                except Exception as e:
                    print(f"[watch] 写入结果文件失败: {e}")

        time.sleep(args.interval)


# =========================
# 主函数
# =========================

def parse_args():
    parser = argparse.ArgumentParser(
        description="512维自编码器ONNX异常检测程序，支持 scaler.json 中的 scale 字段"
    )

    parser.add_argument(
        "--mode",
        type=str,
        default="random",
        choices=["random", "csv", "realtime", "watch"],
        help="运行模式: random / csv / realtime"
    )

    parser.add_argument(
        "--model",
        type=str,
        default=DEFAULT_MODEL_PATH,
        help="ONNX模型路径"
    )

    parser.add_argument(
        "--metadata",
        type=str,
        default=DEFAULT_METADATA_PATH,
        help="metadata.json路径"
    )

    parser.add_argument(
        "--scaler",
        type=str,
        default=DEFAULT_SCALER_PATH,
        help="scaler.json路径"
    )

    parser.add_argument(
        "--csv",
        type=str,
        default=DEFAULT_CSV_PATH,
        help="CSV数据路径"
    )

    parser.add_argument(
        "--csv-format",
        type=str,
        default="series",
        choices=["series", "windows"],
        help="CSV格式: series=一条长时间序列自动切窗; windows=每行一个512维窗口"
    )

    parser.add_argument(
        "--has-header",
        action="store_true",
        help="CSV第一行是否为表头"
    )

    parser.add_argument(
        "--has-label",
        action="store_true",
        help="CSV最后一列是否为标签，仅 csv-format=windows 时使用，0正常，1异常"
    )

    parser.add_argument(
        "--column",
        type=int,
        default=0,
        help="当 csv-format=series 时，读取CSV第几列作为时间序列，从0开始"
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
        help="是否打印输入、标准化输入和输出的前10维"
    )

    parser.add_argument(
        "--data",
        type=str,
        default="/tmp/algo_data.bin",
        help="watch 模式: C 端写出的二进制数据文件路径"
    )

    parser.add_argument(
        "--trigger",
        type=str,
        default="/tmp/algo_trigger.flag",
        help="watch 模式: 触发文件路径，修改时间变化时触发检测"
    )

    parser.add_argument(
        "--stop-file",
        type=str,
        default="/tmp/algo_stop.flag",
        help="watch 模式: 停止文件路径，该文件存在时退出常驻循环"
    )

    parser.add_argument(
        "--max-ch",
        type=int,
        default=8,
        help="watch 模式: 最大通道数"
    )

    parser.add_argument(
        "--max-points",
        type=int,
        default=4096,
        help="watch 模式: 每通道最大点数"
    )

    parser.add_argument(
        "--channel",
        type=int,
        default=3,
        help="watch 模式: 读取第几个通道的数据"
    )

    parser.add_argument(
        "--offset",
        type=int,
        default=0,
        help="watch 模式: 窗口起始偏移"
    )


    parser.add_argument(
        "--result-file",
        type=str,
        default="/tmp/algo_result.txt",
        help="watch 模式: 输出结果文件，格式 algo_id,type,result,timestamp,seq"
    )

    parser.add_argument(
        "--algo-id",
        type=int,
        default=0,
        help="算法编号，由 C 端传入"
    )

    parser.add_argument(
        "--algo-type",
        type=str,
        default="DETECT",
        choices=["DETECT", "DIAG"],
        help="算法类型: DETECT 或 DIAG"
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

    elif args.mode == "watch":
        watch_test(args)