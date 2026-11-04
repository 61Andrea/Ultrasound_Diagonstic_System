import os
import json
import time
import argparse
import numpy as np
import onnxruntime as ort


DEFAULT_MODEL_PATH = "/home/root/model/zz_zd/model/mscnn_model.onnx"
DEFAULT_METADATA_PATH = "/home/root/model/zz_zd/model/metadata.json"
DEFAULT_SCALER_PATH = "/home/root/model/zz_zd/model/scaler.json"
DEFAULT_CSV_PATH = "/home/root/model/zz_zd/data/test_data.csv"


# =========================
# 基础函数
# =========================

def load_json(path):
    if not os.path.exists(path):
        raise FileNotFoundError(f"文件不存在: {path}")

    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def load_metadata(metadata_path):
    meta = load_json(metadata_path)

    seq_len = int(meta.get("seq_len", 512))
    hop = int(meta.get("hop", 128))
    channels = int(meta.get("channels", 1))
    input_layout = meta.get("input_layout", "NCHW")
    num_classes = int(meta.get("num_classes", 4))

    eps = 1e-8
    preprocess = meta.get("preprocess", {})
    preprocess_name = "per_window_zscore"
    if isinstance(preprocess, dict):
        eps = float(preprocess.get("eps", 1e-8))
        preprocess_name = preprocess.get("name", preprocess_name)

    labels_cn = {
        0: "健康",
        1: "轻度",
        2: "中度",
        3: "重度"
    }

    labels_en = {
        0: "Normal",
        1: "Mild",
        2: "Moderate",
        3: "Severe"
    }

    labels = meta.get("labels", {})

    if "id_to_cn" in labels:
        labels_cn = {int(k): v for k, v in labels["id_to_cn"].items()}

    if "id_to_en" in labels:
        labels_en = {int(k): v for k, v in labels["id_to_en"].items()}

    return {
        "seq_len": seq_len,
        "hop": hop,
        "channels": channels,
        "input_layout": input_layout,
        "num_classes": num_classes,
        "eps": eps,
        "preprocess_name": preprocess_name,
        "scaler": None,
        "labels_cn": labels_cn,
        "labels_en": labels_en,
        "raw": meta
    }


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

    C 端数据格式: float algo_data[MAX_CH][MAX_POINTS]
    写入方式: fwrite(algo_data, sizeof(float), MAX_CH * MAX_POINTS, fp)

    参数:
        bin_path:   二进制文件路径，如 /tmp/algo_data.bin
        seq_len:    窗口长度，由 metadata.json 的 seq_len 决定
        max_ch:     最大通道数，默认 8
        max_points: 每通道最大点数，默认 4096
        channel:    要读取的通道号，默认 3
        offset:     窗口起始偏移，默认 0

    返回:
        np.float32 一维数组，长度为 seq_len
    """

    raw = np.fromfile(bin_path, dtype=np.float32)

    expected_size = max_ch * max_points
    if raw.size != expected_size:
        raise ValueError(
            f"二进制文件大小不匹配: 期望 {expected_size} 个float32, 实际 {raw.size}"
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
# 预处理
# =========================

def load_standard_scaler(scaler_path, seq_len, eps=1e-8):
    scaler = load_json(scaler_path)

    if "mean" not in scaler or "scale" not in scaler:
        raise ValueError("scaler文件格式错误: 需要包含 mean 和 scale")

    mean = np.array(scaler["mean"], dtype=np.float32).reshape(-1)
    scale = np.array(scaler["scale"], dtype=np.float32).reshape(-1)

    if mean.shape[0] != seq_len:
        raise ValueError(
            f"scaler mean长度错误: 当前长度={mean.shape[0]}, 模型需要={seq_len}"
        )

    if scale.shape[0] != seq_len:
        raise ValueError(
            f"scaler scale长度错误: 当前长度={scale.shape[0]}, 模型需要={seq_len}"
        )

    if np.any(np.abs(scale) < eps):
        raise ValueError("scaler scale中存在接近0的值，无法安全标准化")

    return {
        "mean": mean,
        "scale": scale
    }


def load_runtime_config(args):
    cfg = load_metadata(args.metadata)
    preprocess_name = str(cfg.get("preprocess_name", "per_window_zscore")).lower()

    if preprocess_name == "standard_scaler_embedded":
        # StandardScaler 已嵌入 ONNX 模型，无需加载外部 scaler
        pass
    elif preprocess_name == "standard_scaler":
        cfg["scaler"] = load_standard_scaler(
            args.scaler,
            seq_len=cfg["seq_len"],
            eps=cfg["eps"]
        )

    return cfg

def per_window_zscore(window, eps=1e-8):
    """
    每个窗口单独做 z-score:
        x_norm = (x - mean) / std

    输入:
        window shape = [seq_len]

    输出:
        window_norm shape = [seq_len]
    """

    window = np.array(window, dtype=np.float32).reshape(-1)

    mu = np.mean(window)
    sigma = np.std(window)

    if sigma < eps:
        sigma = eps

    window_norm = (window - mu) / sigma

    return window_norm.astype(np.float32)


def standard_scaler_transform(window, scaler):
    window = np.array(window, dtype=np.float32).reshape(-1)
    return ((window - scaler["mean"]) / scaler["scale"]).astype(np.float32)


def preprocess_window(window, cfg):
    preprocess_name = str(cfg.get("preprocess_name", "per_window_zscore")).lower()

    if preprocess_name == "standard_scaler_embedded":
        # 预处理已嵌入 ONNX，直接透传原始加速度窗口
        return np.array(window, dtype=np.float32).reshape(-1)

    if preprocess_name == "standard_scaler":
        scaler = cfg.get("scaler")
        if scaler is None:
            raise ValueError("metadata要求standard_scaler，但未加载scaler文件")
        return standard_scaler_transform(window, scaler)

    return per_window_zscore(window, eps=cfg["eps"])


def make_model_input(window_norm, input_layout="NCHW"):
    """
    MultiScaleCNN 输入通常是:
        NCHW: [batch, channel, length] = [1, 1, 512]

    注意：虽然名字叫 NCHW，但一维卷积模型常见实际输入是 [N, C, L]
    """

    window_norm = np.array(window_norm, dtype=np.float32).reshape(1, 1, -1)

    if input_layout.upper() == "NCHW":
        return window_norm

    if input_layout.upper() == "NCW":
        return window_norm

    raise ValueError(f"暂不支持 input_layout={input_layout}")


def softmax(x):
    """
    如果模型输出是logits，则用softmax转概率。
    如果模型已经输出概率，softmax后通常也仍然能得到合理排序。
    """

    x = np.array(x, dtype=np.float32)
    x = x - np.max(x, axis=-1, keepdims=True)
    exp_x = np.exp(x)
    return exp_x / np.sum(exp_x, axis=-1, keepdims=True)


# =========================
# 单窗口推理
# =========================

def infer_window(session, input_name, output_name, window_raw, cfg):
    seq_len = cfg["seq_len"]
    input_layout = cfg["input_layout"]

    window_raw = np.array(window_raw, dtype=np.float32).reshape(-1)

    if window_raw.shape[0] != seq_len:
        raise ValueError(
            f"窗口长度错误: 当前长度={window_raw.shape[0]}, 模型需要={seq_len}"
        )

    window_norm = preprocess_window(window_raw, cfg)

    x = make_model_input(window_norm, input_layout=input_layout)

    y = session.run([output_name], {input_name: x})[0]
    y = np.array(y, dtype=np.float32)

    # 常见输出 shape: [1, 4]
    y = y.reshape(1, -1)

    probs = softmax(y)[0]
    pred_id = int(np.argmax(probs))
    confidence = float(probs[pred_id])

    return {
        "pred_id": pred_id,
        "confidence": confidence,
        "probs": probs,
        "raw_output": y[0],
        "window_mean": float(np.mean(window_raw)),
        "window_std": float(np.std(window_raw)),
        "window_norm": window_norm
    }


# =========================
# 随机数据测试
# =========================

def random_test(args):
    cfg = load_runtime_config(args)
    session, input_name, output_name = create_onnx_session(args.model)

    seq_len = cfg["seq_len"]
    labels_cn = cfg["labels_cn"]
    labels_en = cfg["labels_en"]

    print("========== 配置信息 ==========")
    print("seq_len:", seq_len)
    print("hop:", cfg["hop"])
    print("num_classes:", cfg["num_classes"])
    print("preprocess:", cfg["preprocess_name"])
    print("==============================")
    print("开始随机数据测试")

    for i in range(args.loop):
        # 生成一段模拟振动信号
        # 这里用正弦 + 噪声，便于测试程序能不能跑通
        t = np.arange(seq_len, dtype=np.float32)
        signal = (
            np.sin(2.0 * np.pi * 5.0 * t / seq_len)
            + 0.2 * np.random.randn(seq_len)
        ).astype(np.float32)

        result = infer_window(
            session=session,
            input_name=input_name,
            output_name=output_name,
            window_raw=signal,
            cfg=cfg
        )

        pred_id = result["pred_id"]

        print("----------")
        print(f"第 {i + 1} 次随机检测")
        print("预测类别ID:", pred_id)
        print("中文类别:", labels_cn.get(pred_id, str(pred_id)))
        print("英文类别:", labels_en.get(pred_id, str(pred_id)))
        print("置信度:", f"{result['confidence']:.6f}")
        print("四类概率:", result["probs"])
        print("窗口均值:", f"{result['window_mean']:.6f}")
        print("窗口标准差:", f"{result['window_std']:.6f}")

        if args.print_data:
            print("原始数据前10点:", signal[:10])
            print("标准化后前10点:", result["window_norm"][:10])
            print("模型原始输出:", result["raw_output"])

        time.sleep(args.interval)


# =========================
# CSV读取与切窗
# =========================

def read_csv_series(csv_path, has_header=False, column=0):
    """
    读取CSV时间序列。

    支持几种情况：

    1. CSV只有一列：
        123.4
        125.6
        128.9

    2. CSV有多列，选择某一列：
        X,Y,Z
        1.1,2.2,3.3
        1.2,2.3,3.4

    参数 column 表示选择第几列，从0开始。
    例如:
        column=0 读取第一列
        column=1 读取第二列
        column=2 读取第三列
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


def split_windows(series, seq_len, hop):
    """
    按 seq_len 和 hop 切窗。

    例如:
        seq_len = 512
        hop = 128

    窗口:
        0:    [0, 512)
        1:    [128, 640)
        2:    [256, 768)
    """

    series = np.array(series, dtype=np.float32).reshape(-1)

    if series.shape[0] < seq_len:
        raise ValueError(
            f"CSV数据长度不足: 当前长度={series.shape[0]}, 至少需要={seq_len}"
        )

    windows = []
    starts = []

    for start in range(0, series.shape[0] - seq_len + 1, hop):
        end = start + seq_len
        windows.append(series[start:end])
        starts.append(start)

    windows = np.array(windows, dtype=np.float32)

    return windows, starts


def csv_test(args):
    cfg = load_runtime_config(args)
    session, input_name, output_name = create_onnx_session(args.model)

    seq_len = cfg["seq_len"]
    hop = cfg["hop"]
    labels_cn = cfg["labels_cn"]
    labels_en = cfg["labels_en"]
    num_classes = cfg["num_classes"]

    series = read_csv_series(
        csv_path=args.csv,
        has_header=args.has_header,
        column=args.column
    )

    windows, starts = split_windows(series, seq_len, hop)

    print("========== CSV信息 ==========")
    print("CSV路径:", args.csv)
    print("读取列 column:", args.column)
    print("时间序列长度:", series.shape[0])
    print("窗口长度 seq_len:", seq_len)
    print("滑动步长 hop:", hop)
    print("窗口数量:", windows.shape[0])
    print("============================")
    print("开始CSV窗口检测")

    class_counts = np.zeros(num_classes, dtype=np.int64)
    confidence_sum = np.zeros(num_classes, dtype=np.float64)

    all_results = []

    for i in range(windows.shape[0]):
        window = windows[i]

        result = infer_window(
            session=session,
            input_name=input_name,
            output_name=output_name,
            window_raw=window,
            cfg=cfg
        )

        pred_id = result["pred_id"]
        confidence = result["confidence"]

        class_counts[pred_id] += 1
        confidence_sum[pred_id] += confidence

        all_results.append(result)

        print(
            f"窗口 {i + 1:04d}, "
            f"start={starts[i]}, "
            f"pred={pred_id}, "
            f"cn={labels_cn.get(pred_id, str(pred_id))}, "
            f"en={labels_en.get(pred_id, str(pred_id))}, "
            f"conf={confidence:.6f}, "
            f"probs={np.round(result['probs'], 6)}"
        )

        if args.print_data:
            print("原始窗口前10点:", window[:10])
            print("标准化后前10点:", result["window_norm"][:10])
            print("模型原始输出:", result["raw_output"])

    # 整条CSV最终结果：按窗口多数投票
    final_pred = int(np.argmax(class_counts))
    final_cn = labels_cn.get(final_pred, str(final_pred))
    final_en = labels_en.get(final_pred, str(final_pred))

    print("============================")
    print("CSV检测完成")
    print("各类别窗口数量:")

    for cid in range(num_classes):
        count = int(class_counts[cid])
        ratio = count / windows.shape[0]
        avg_conf = 0.0
        if count > 0:
            avg_conf = confidence_sum[cid] / count

        print(
            f"类别 {cid}: "
            f"{labels_cn.get(cid, str(cid))} / {labels_en.get(cid, str(cid))}, "
            f"数量={count}, "
            f"占比={ratio * 100:.2f}%, "
            f"平均置信度={avg_conf:.6f}"
        )

    print("----------------------------")
    print("整条CSV最终诊断结果:")
    print("类别ID:", final_pred)
    print("中文:", final_cn)
    print("英文:", final_en)
    print("依据: 窗口多数投票")


# =========================
# 实时数据接口
# =========================

def get_realtime_window(seq_len):
    """
    预留实时采集接口。

    后续你接自己的数据时，只需要改这里。

    要求返回:
        一维数组，长度 seq_len，例如 512 个点。

    示例:
        data = read_from_dma()
        return np.array(data, dtype=np.float32).reshape(seq_len)

    注意:
        这里返回的是原始数据，不要提前z-score。
        程序会在 infer_window() 里自动做标准化预处理。
    """

    raise NotImplementedError("请在 get_realtime_window() 中接入你的真实采集数据")


def realtime_test(args):
    cfg = load_runtime_config(args)
    session, input_name, output_name = create_onnx_session(args.model)

    seq_len = cfg["seq_len"]
    labels_cn = cfg["labels_cn"]
    labels_en = cfg["labels_en"]

    print("========== 实时检测 ==========")
    print("seq_len:", seq_len)
    print("preprocess:", cfg["preprocess_name"])
    print("==============================")

    index = 0

    while True:
        index += 1

        window = get_realtime_window(seq_len)

        result = infer_window(
            session=session,
            input_name=input_name,
            output_name=output_name,
            window_raw=window,
            cfg=cfg
        )

        pred_id = result["pred_id"]

        print(
            f"第 {index} 次实时检测: "
            f"pred={pred_id}, "
            f"cn={labels_cn.get(pred_id, str(pred_id))}, "
            f"en={labels_en.get(pred_id, str(pred_id))}, "
            f"conf={result['confidence']:.6f}, "
            f"probs={np.round(result['probs'], 6)}"
        )

        time.sleep(args.interval)


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


# =========================
# 连续检测 watch 模式
# =========================

def watch_test(args):
    """
    Python 常驻进程，连续监听 C 端写出的数据文件并完成推理。

    工作流程:
        1. 启动时加载 metadata 和 ONNX 模型（只加载一次）。
        2. 循环监听 trigger 文件（默认 /tmp/algo_trigger.flag）。
        3. 当 trigger 文件的修改时间变化时，读取 data 文件（默认 /tmp/algo_data.bin）。
        4. 使用 read_bin_window() 读取窗口，调用 infer_window() 推理。
        5. 输出检测结果，写入 /tmp/algo_last_result.txt。
        6. 如果检测到 stop_file（默认 /tmp/algo_stop.flag），退出循环。
    """

    cfg = load_runtime_config(args)
    session, input_name, output_name = create_onnx_session(args.model)

    seq_len = cfg["seq_len"]
    labels_cn = cfg["labels_cn"]
    labels_en = cfg["labels_en"]

    print("========== 连续检测 watch 模式 ==========")
    print("数据文件:", args.data)
    print("触发文件:", args.trigger)
    print("停止文件:", args.stop_file)
    print("结果文件:", args.result_file)
    print("结果流文件:", args.result_stream_file)
    print("algo_id:", args.algo_id)
    print("algo_type:", args.algo_type)
    print("max_ch:", args.max_ch)
    print("max_points:", args.max_points)
    print("channel:", args.channel)
    print("offset:", args.offset)
    print("seq_len:", seq_len)
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
                seq, run_id = read_trigger_context(args.trigger)

                # 读取数据窗口
                try:
                    window = read_bin_window(
                        bin_path=args.data,
                        seq_len=seq_len,
                        max_ch=args.max_ch,
                        max_points=args.max_points,
                        channel=args.channel,
                        offset=args.offset
                    )
                 # index在推理完成后才加1，因此index==1表示当前是第二包
                    if index == 1:
                        snapshot = "/tmp/algo7_watch_512_second.csv"

                        np.savetxt(
                            snapshot,
                            window.reshape(-1, 1),
                            delimiter=",",
                            fmt="%.9g"
                        )

                        print(
                            "[watch第二包]",
                            "file=", snapshot,
                            "count=", window.size,
                            "min=", float(window.min()),
                            "max=", float(window.max()),
                            "mean=", float(window.mean()),
                            "std=", float(window.std()),
                            "first10=", window[:10],
                            flush=True
                        )   
                except Exception as e:
                    print(f"[watch] 读取数据失败: {e}")
                    time.sleep(args.interval)
                    continue

                # 推理
                result = infer_window(
                    session=session,
                    input_name=input_name,
                    output_name=output_name,
                    window_raw=window,
                    cfg=cfg
                )

                index += 1
                pred_id = result["pred_id"]

                print(
                    f"[watch #{index}] "
                    f"pred={pred_id}, "
                    f"cn={labels_cn.get(pred_id, str(pred_id))}, "
                    f"en={labels_en.get(pred_id, str(pred_id))}, "
                    f"conf={result['confidence']:.6f}, "
                    f"probs={np.round(result['probs'], 6)}"
                )

                timestamp = int(time.time())

                # 写入本算法独立结果文件，供 C 查询并返回给上位机
                try:
                    with open(args.result_file, "w") as f:
                        f.write(
                            f"{args.algo_id},{args.algo_type},{pred_id},{timestamp},{seq}\n"
                        )
                    append_watch_result(
                        args.result_stream_file, run_id, args.algo_id,
                        args.algo_type, pred_id, seq
                    )
                except Exception as e:
                    print(f"[watch] 写入结果文件失败: {e}")

        time.sleep(args.interval)


# =========================
# 主函数
# =========================

def parse_args():
    parser = argparse.ArgumentParser(
        description="MultiScaleCNN 4分类故障诊断 ONNX检测程序"
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
        help="StandardScaler参数JSON路径"
    )

    parser.add_argument(
        "--csv",
        type=str,
        default=DEFAULT_CSV_PATH,
        help="CSV数据路径"
    )

    parser.add_argument(
        "--has-header",
        action="store_true",
        help="CSV第一行是否是表头"
    )

    parser.add_argument(
        "--column",
        type=int,
        default=0,
        help="读取CSV第几列作为时间序列，从0开始。Z轴如果在第三列，一般用 --column 2"
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
        help="是否打印窗口前10点和模型原始输出"
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
        "--result-file",
        type=str,
        default="/tmp/algo_result.txt",
        help="watch 模式: 结果输出文件，格式 algo_id,algo_type,result_code,timestamp,seq"
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
        default=7,
        help="算法编号"
    )

    parser.add_argument(
        "--algo-type",
        type=str,
        default="DIAG",
        help="算法类型: DETECT 或 DIAG"
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
