import os
import json
import time
import argparse
import numpy as np
import onnxruntime as ort


# =========================
# 默认路径
# =========================

DEFAULT_MODEL_PATH = "/home/root/model/model.onnx"
DEFAULT_METADATA_PATH = "/home/root/model/metadata.json"
DEFAULT_CSV_PATH = "/home/root/model/test_data.csv"


# =========================
# 基础工具函数
# =========================

def load_json(path):
    if not os.path.exists(path):
        raise FileNotFoundError(f"文件不存在: {path}")

    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def softmax(x):
    x = np.array(x, dtype=np.float32)
    x = x - np.max(x, axis=-1, keepdims=True)
    exp_x = np.exp(x)
    return exp_x / np.sum(exp_x, axis=-1, keepdims=True)


# =========================
# 加载 metadata
# =========================

def load_metadata(metadata_path):
    meta = load_json(metadata_path)

    in_channels = int(meta.get("in_channels", 8))
    window_size = int(meta.get("window_size", 32))
    num_classes = int(meta.get("num_classes", 5))

    class_names = meta.get("class_names", None)
    if class_names is None:
        class_names = [f"class_{i}" for i in range(num_classes)]

    mean = meta.get("mean", None)
    std = meta.get("std", None)

    if mean is None:
        print("警告: metadata.json 中没有 mean，默认使用全0")
        mean = [0.0] * in_channels

    if std is None:
        print("警告: metadata.json 中没有 std，默认使用全1")
        std = [1.0] * in_channels

    mean = np.array(mean, dtype=np.float32).reshape(1, in_channels, 1)
    std = np.array(std, dtype=np.float32).reshape(1, in_channels, 1)

    if mean.shape[1] != in_channels:
        raise ValueError(
            f"mean维度错误: mean.shape={mean.shape}, in_channels={in_channels}"
        )

    if std.shape[1] != in_channels:
        raise ValueError(
            f"std维度错误: std.shape={std.shape}, in_channels={in_channels}"
        )

    std = np.where(std == 0, 1.0, std).astype(np.float32)

    print("metadata 加载成功")
    print("in_channels:", in_channels)
    print("window_size:", window_size)
    print("num_classes:", num_classes)
    print("class_names:", class_names)
    print("mean shape:", mean.shape)
    print("std shape:", std.shape)

    return {
        "in_channels": in_channels,
        "window_size": window_size,
        "num_classes": num_classes,
        "class_names": class_names,
        "mean": mean,
        "std": std,
        "raw": meta
    }


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
# 数据预处理
# =========================

def normalize_window(x, mean, std):
    """
    对 8 通道数据做标准化。

    输入 x shape:
        [1, 8, 32]

    mean/std shape:
        [1, 8, 1]

    标准化：
        x_norm[:, c, :] = (x[:, c, :] - mean[c]) / std[c]
    """
    return (x - mean) / std


def make_model_input_from_window(window, cfg):
    """
    把窗口数据整理成模型输入 shape: [1, 8, 32]

    支持两种输入 window:
        1. shape = [32, 8]，即32行时间点，每行8通道
        2. shape = [8, 32]，即8通道，每通道32点

    最终统一转成:
        [1, 8, 32]
    """

    in_channels = cfg["in_channels"]
    window_size = cfg["window_size"]

    window = np.array(window, dtype=np.float32)

    if window.shape == (window_size, in_channels):
        # [32, 8] -> [8, 32]
        window = window.T
    elif window.shape == (in_channels, window_size):
        # 已经是 [8, 32]
        pass
    else:
        raise ValueError(
            f"窗口shape错误: 当前shape={window.shape}, "
            f"期望 [window_size, in_channels]=[{window_size}, {in_channels}] "
            f"或 [in_channels, window_size]=[{in_channels}, {window_size}]"
        )

    x = window.reshape(1, in_channels, window_size).astype(np.float32)
    return x


# =========================
# 单窗口推理
# =========================

def infer_window(session, input_name, output_name, window, cfg):
    """
    单个窗口分类。

    流程:
        原始窗口 [32,8] 或 [8,32]
        -> [1,8,32]
        -> 按通道 mean/std 标准化
        -> ONNX推理
        -> softmax
        -> argmax得到类别
    """

    x_raw = make_model_input_from_window(window, cfg)

    x_norm = normalize_window(
        x_raw,
        cfg["mean"],
        cfg["std"]
    ).astype(np.float32)

    logits = session.run([output_name], {input_name: x_norm})[0]
    logits = np.array(logits, dtype=np.float32).reshape(1, -1)

    if logits.shape[1] != cfg["num_classes"]:
        raise ValueError(
            f"模型输出类别数不匹配: output={logits.shape}, "
            f"metadata num_classes={cfg['num_classes']}"
        )

    probs = softmax(logits)[0]
    pred_id = int(np.argmax(probs))
    confidence = float(probs[pred_id])

    return {
        "pred_id": pred_id,
        "class_name": cfg["class_names"][pred_id],
        "confidence": confidence,
        "probs": probs,
        "logits": logits[0],
        "x_raw": x_raw,
        "x_norm": x_norm
    }


# =========================
# 随机数据测试
# =========================

def random_test(args):
    cfg = load_metadata(args.metadata)
    session, input_name, output_name = create_onnx_session(args.model)

    in_channels = cfg["in_channels"]
    window_size = cfg["window_size"]

    print("==============================")
    print("开始随机数据测试")
    print("==============================")

    for i in range(args.loop):
        # 生成接近训练数据分布的随机数据
        # x_norm 是标准正态，反标准化得到 x_raw
        x_norm = np.random.randn(1, in_channels, window_size).astype(np.float32)
        x_raw = x_norm * cfg["std"] + cfg["mean"]

        # infer_window 支持 [8,32]，所以传 x_raw[0]
        result = infer_window(
            session=session,
            input_name=input_name,
            output_name=output_name,
            window=x_raw[0],
            cfg=cfg
        )

        print("----------")
        print(f"第 {i + 1} 次随机检测")
        print("预测类别ID:", result["pred_id"])
        print("预测类别:", result["class_name"])
        print("置信度:", f"{result['confidence']:.6f}")
        print("五类概率:", np.round(result["probs"], 6))

        if args.print_data:
            print("logits:", result["logits"])
            print("原始输入 shape:", result["x_raw"].shape)
            print("标准化输入 shape:", result["x_norm"].shape)
            print("第1通道原始前10点:", result["x_raw"][0, 0, :10])
            print("第1通道标准化前10点:", result["x_norm"][0, 0, :10])

        time.sleep(args.interval)


# =========================
# CSV读取与切窗
# =========================

def read_csv_array(csv_path, has_header=False):
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
        data = data.reshape(1, -1)

    if data.ndim != 2:
        raise ValueError(f"CSV数据维度异常: shape={data.shape}")

    return data


def split_series_windows(data, window_size, hop):
    """
    CSV格式为长时间序列:
        每一行 = 一个时间点
        每一行有8列 = 8个通道

    data shape:
        [N, 8]

    切窗后:
        windows shape:
        [num_windows, 32, 8]
    """

    n = data.shape[0]

    if n < window_size:
        raise ValueError(
            f"CSV行数不足: 当前行数={n}, 至少需要 window_size={window_size}"
        )

    windows = []
    starts = []

    for start in range(0, n - window_size + 1, hop):
        end = start + window_size
        windows.append(data[start:end, :])
        starts.append(start)

    windows = np.array(windows, dtype=np.float32)

    return windows, starts


def read_windows_csv(data, cfg, has_label=False):
    """
    CSV格式为每行一个窗口。

    支持：
        1. 每行 256 个特征，即 8*32
        2. 每行 257 列，最后一列为标签

    默认认为每行排列方式是：
        ch1的32个点, ch2的32个点, ..., ch8的32个点

    即 reshape 成 [8, 32]。
    """

    in_channels = cfg["in_channels"]
    window_size = cfg["window_size"]
    feature_dim = in_channels * window_size

    labels = None

    if has_label:
        labels = data[:, -1].astype(np.int64)
        data = data[:, :-1]

    if data.shape[1] != feature_dim:
        raise ValueError(
            f"CSV窗口特征数错误: 当前列数={data.shape[1]}, "
            f"模型需要 {feature_dim} 列，也就是 {in_channels}*{window_size}。"
            f"如果最后一列是标签，请加 --has-label。"
        )

    windows = data.reshape(-1, in_channels, window_size).astype(np.float32)
    starts = list(range(windows.shape[0]))

    return windows, starts, labels


def csv_test(args):
    cfg = load_metadata(args.metadata)
    session, input_name, output_name = create_onnx_session(args.model)

    data = read_csv_array(args.csv, has_header=args.has_header)

    in_channels = cfg["in_channels"]
    window_size = cfg["window_size"]

    if args.csv_format == "series":
        # 每行一个时间点，每行8列
        if data.shape[1] != in_channels:
            raise ValueError(
                f"CSV列数错误: csv-format=series 时，每行应该有 {in_channels} 列，"
                f"当前CSV有 {data.shape[1]} 列。"
            )

        windows, starts = split_series_windows(
            data=data,
            window_size=window_size,
            hop=args.hop
        )
        labels = None

        print("==============================")
        print("CSV格式: 长时间序列")
        print("每行 = 一个时间点")
        print("每行列数 =", data.shape[1])
        print("自动切窗 shape:", windows.shape)
        print("==============================")

    elif args.csv_format == "windows":
        # 每行一个窗口，每行 8*32=256 列
        windows, starts, labels = read_windows_csv(
            data=data,
            cfg=cfg,
            has_label=args.has_label
        )

        print("==============================")
        print("CSV格式: 每行一个窗口")
        print("windows shape:", windows.shape)
        print("==============================")

    else:
        raise ValueError("csv_format 只能是 series 或 windows")

    class_counts = np.zeros(cfg["num_classes"], dtype=np.int64)
    correct_count = 0
    has_label = labels is not None

    print("开始CSV检测")

    for i in range(windows.shape[0]):
        window = windows[i]

        result = infer_window(
            session=session,
            input_name=input_name,
            output_name=output_name,
            window=window,
            cfg=cfg
        )

        pred_id = result["pred_id"]
        class_counts[pred_id] += 1

        line = (
            f"窗口 {i + 1:04d}, "
            f"start={starts[i]}, "
            f"pred={pred_id}, "
            f"class={result['class_name']}, "
            f"conf={result['confidence']:.6f}, "
            f"probs={np.round(result['probs'], 6)}"
        )

        if has_label:
            label = int(labels[i])
            if pred_id == label:
                correct_count += 1
            line += f", label={label}"

        print(line)

        if args.print_data:
            print("logits:", result["logits"])
            print("输入shape:", result["x_raw"].shape)
            print("第1通道原始前10点:", result["x_raw"][0, 0, :10])
            print("第1通道标准化前10点:", result["x_norm"][0, 0, :10])

    print("==============================")
    print("CSV检测完成")
    print("各类别窗口数量:")

    total = windows.shape[0]

    for cid in range(cfg["num_classes"]):
        count = int(class_counts[cid])
        ratio = count / total if total > 0 else 0.0
        name = cfg["class_names"][cid]
        print(
            f"类别 {cid}: {name}, "
            f"数量={count}, "
            f"占比={ratio * 100:.2f}%"
        )

    final_pred = int(np.argmax(class_counts))
    final_name = cfg["class_names"][final_pred]

    print("------------------------------")
    print("整条CSV最终判断:")
    print("类别ID:", final_pred)
    print("类别名称:", final_name)
    print("依据: 窗口多数投票")

    if has_label:
        acc = correct_count / total if total > 0 else 0.0
        print("------------------------------")
        print("标签正确数量:", correct_count)
        print("准确率:", f"{acc * 100:.2f}%")


# =========================
# 实时数据接口
# =========================

def get_realtime_window(cfg):
    """
    预留实时采集接口。

    要求返回 shape = [32, 8] 或 [8, 32]。

    推荐返回 [32, 8]：
        32行时间点，每行8个通道。

    示例：
        data = read_from_dma()
        data = np.array(data, dtype=np.float32).reshape(32, 8)
        return data

    注意：
        这里返回原始数据，不要提前标准化。
        程序会自动按 metadata.json 里的 mean/std 标准化。
    """

    raise NotImplementedError("请在 get_realtime_window() 中接入你的真实采集数据")


def realtime_test(args):
    cfg = load_metadata(args.metadata)
    session, input_name, output_name = create_onnx_session(args.model)

    print("==============================")
    print("开始实时检测")
    print("输入要求: [32, 8] 或 [8, 32]")
    print("==============================")

    index = 0

    while True:
        index += 1

        window = get_realtime_window(cfg)

        result = infer_window(
            session=session,
            input_name=input_name,
            output_name=output_name,
            window=window,
            cfg=cfg
        )

        print(
            f"第 {index} 次实时检测: "
            f"pred={result['pred_id']}, "
            f"class={result['class_name']}, "
            f"conf={result['confidence']:.6f}, "
            f"probs={np.round(result['probs'], 6)}"
        )

        time.sleep(args.interval)




# =========================
# C端二进制数据读取与结果写入
# =========================

def read_trigger_context(trigger_path):
    """读取 C 端写入 trigger 文件中的 seq,run_id。"""
    try:
        with open(trigger_path, "r", encoding="utf-8") as f:
            first = f.readline().strip()
        parts = first.split(",", 1)
        seq = int(parts[0]) if parts[0] else 0
        run_id = int(parts[1]) if len(parts) > 1 and parts[1] else 0
        return seq, run_id
    except Exception:
        return 0, 0


def write_watch_result(result_file, algo_id, algo_type, result_code, seq):
    """统一结果格式: algo_id,algo_type,result_code,timestamp,seq"""
    timestamp = int(time.time())
    tmp_file = result_file + ".tmp"
    with open(tmp_file, "w", encoding="utf-8") as f:
        f.write(f"{int(algo_id)},{algo_type},{int(result_code)},{timestamp},{int(seq)}\n")
    os.replace(tmp_file, result_file)


def append_watch_result(result_stream_file, run_id, algo_id, algo_type,
                        result_code, seq):
    """追加一条完整结果，格式为 run_id,algo_id,type,result,timestamp,seq。"""
    if not result_stream_file:
        return

    timestamp = int(time.time())
    line = (
        f"{int(run_id)},{int(algo_id)},{algo_type},"
        f"{int(result_code)},{timestamp},{int(seq)}\n"
    ).encode("utf-8")
    fd = os.open(
        result_stream_file,
        os.O_WRONLY | os.O_CREAT | os.O_APPEND,
        0o644
    )
    try:
        written = os.write(fd, line)
        if written != len(line):
            raise OSError(f"结果流短写: 期望{len(line)}字节, 实际{written}字节")
    finally:
        os.close(fd)


def read_bin_window_1ch(bin_path, seq_len, max_ch=8, max_points=4096, channel=3, offset=0):
    """读取 C 端 float algo_data[MAX_CH][MAX_POINTS] 中指定单通道窗口。"""
    raw = np.fromfile(bin_path, dtype=np.float32)
    expected_size = max_ch * max_points
    if raw.size != expected_size:
        raise ValueError(f"二进制文件大小不匹配: 期望 {expected_size} 个float32, 实际 {raw.size}")
    data = raw.reshape(max_ch, max_points)
    if channel < 0 or channel >= max_ch:
        raise ValueError(f"channel={channel} 超出范围 [0, {max_ch - 1}]")
    if offset + seq_len > max_points:
        raise ValueError(f"offset({offset}) + seq_len({seq_len}) 超出 max_points({max_points})")
    return data[channel, offset:offset + seq_len].astype(np.float32)


def read_bin_window_8ch32(bin_path, max_ch=8, max_points=4096,
                          channel=3, offset=0, window_size=32):
    """读取 C 端单个逻辑通道内按通道优先展开的 8x32 float 窗口。"""
    raw = np.fromfile(bin_path, dtype=np.float32)
    expected_size = max_ch * max_points
    if raw.size != expected_size:
        raise ValueError(f"二进制文件大小不匹配: 期望 {expected_size} 个float32, 实际 {raw.size}")
    data = raw.reshape(max_ch, max_points)
    if channel < 0 or channel >= max_ch:
        raise ValueError(f"channel={channel} 超出范围 [0, {max_ch - 1}]")

    feature_count = 8 * window_size
    if offset + feature_count > max_points:
        raise ValueError(
            f"offset({offset}) + 8*window_size({feature_count}) "
            f"超出 max_points({max_points})"
        )

    flat = data[channel, offset:offset + feature_count]
    return flat.reshape(8, window_size).astype(np.float32)


# =========================
# 连续检测 watch 模式
# =========================

def watch_test(args):
    cfg = load_metadata(args.metadata)
    session, input_name, output_name = create_onnx_session(args.model)
    window_size = cfg["window_size"]

    print("========== 连续检测 watch 模式 ==========")
    print("数据文件:", args.data)
    print("触发文件:", args.trigger)
    print("停止文件:", args.stop_file)
    print("结果文件:", args.result_file)
    print("结果流文件:", args.result_stream_file)
    print("algo_id:", args.algo_id)
    print("algo_type:", args.algo_type)
    print("window_size:", window_size)
    print("offset:", args.offset)
    print("========================================")

    last_mtime = 0
    index = 0
    while True:
        if os.path.exists(args.stop_file):
            print("[watch] 检测到停止文件，退出")
            break
        if os.path.exists(args.trigger):
            try:
                current_mtime = os.path.getmtime(args.trigger)
            except OSError:
                current_mtime = 0
            if current_mtime != last_mtime:
                last_mtime = current_mtime
                seq, run_id = read_trigger_context(args.trigger)
                try:
                    window = read_bin_window_8ch32(
                        args.data, args.max_ch, args.max_points,
                        args.channel, args.offset, window_size
                    )
                    result = infer_window(session, input_name, output_name, window, cfg)
                    result_code = int(result["pred_id"])
                    index += 1
                    print(f"[watch #{index}] pred={result_code}, class={result['class_name']}, conf={result['confidence']:.6f}, seq={seq}")
                    write_watch_result(args.result_file, args.algo_id, args.algo_type, result_code, seq)
                    append_watch_result(
                        args.result_stream_file, run_id, args.algo_id,
                        args.algo_type, result_code, seq
                    )
                except Exception as e:
                    print(f"[watch] 检测失败: {e}")
        time.sleep(args.interval)

# =========================
# 主函数
# =========================

def parse_args():
    parser = argparse.ArgumentParser(
        description="8通道32窗口CNN 5分类ONNX检测程序"
    )

    parser.add_argument(
        "--mode",
        type=str,
        default="random",
        choices=["random", "csv", "realtime", "watch"],
        help="运行模式: random / csv / realtime / watch"
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
        help="CSV格式: series=每行一个时间点8通道; windows=每行一个完整窗口256特征"
    )

    parser.add_argument(
        "--has-header",
        action="store_true",
        help="CSV第一行是否为表头"
    )

    parser.add_argument(
        "--has-label",
        action="store_true",
        help="CSV最后一列是否为标签，仅 csv-format=windows 时使用"
    )

    parser.add_argument(
        "--hop",
        type=int,
        default=32,
        help="csv-format=series 时的滑动步长，默认32表示不重叠"
    )

    parser.add_argument(
        "--loop",
        type=int,
        default=5,
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
        help="是否打印部分输入、标准化数据和logits"
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
        help="watch 模式: 触发文件路径，内容为 C 端递增 seq"
    )

    parser.add_argument(
        "--stop-file",
        type=str,
        default="/tmp/algo_stop.flag",
        help="watch 模式: 停止文件路径，该文件存在时退出常驻循环"
    )

    parser.add_argument(
        "--result-file",
        type=str,
        default="/tmp/algo_result.txt",
        help="watch 模式: 输出结果文件，格式 algo_id,type,result,timestamp,seq"
    )

    parser.add_argument(
        "--result-stream-file",
        type=str,
        default="",
        help="watch 模式: 追加结果流，格式 run_id,algo_id,type,result,timestamp,seq"
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
        default="DIAG",
        choices=["DETECT", "DIAG"],
        help="算法类型: DETECT 或 DIAG"
    )

    parser.add_argument(
        "--max-ch",
        type=int,
        default=8,
        help="watch 模式: C 端二进制数据最大通道数"
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
        help="watch 模式: 读取第几个通道，从0开始"
    )

    parser.add_argument(
        "--offset",
        type=int,
        default=0,
        help="watch 模式: 窗口起始偏移"
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