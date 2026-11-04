import os
import json
import time
import argparse
import numpy as np
import onnxruntime as ort


# =========================
# 默认路径
# =========================

DEFAULT_MODEL_PATH = "/home/root/model/bearing_zd/model/multiscale_cnn.onnx"
DEFAULT_METADATA_PATH = "/home/root/model/bearing_zd/model/metadata.json"
DEFAULT_SCALER_PATH = "/home/root/model/bearing_zd/model/scaler.json"
DEFAULT_CSV_PATH = "/home/root/model/data/Normal.csv"
DEFAULT_RESULT_FILE = "/tmp/algo_result.txt"
# =========================
# 基础函数
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


def to_2d_float32(arr):
    arr = np.array(arr, dtype=np.float32)

    if arr.ndim == 1:
        arr = arr.reshape(1, -1)

    if arr.ndim != 2:
        raise ValueError(f"期望二维数组，实际 shape={arr.shape}")

    return arr


# =========================
# 加载配置
# =========================

def load_config(metadata_path, scaler_path):
    meta = load_json(metadata_path)
    scaler = load_json(scaler_path)

    input_length = meta.get("input_length", None)
    if input_length is None:
        input_length = meta.get("input_dim", None)

    if input_length is None:
        raise ValueError("metadata.json 中没有 input_length 或 input_dim")

    input_length = int(input_length)

    num_classes = meta.get("num_classes", None)
    if num_classes is None:
        raise ValueError("metadata.json 中没有 num_classes")

    num_classes = int(num_classes)

    class_names = meta.get("class_names", None)
    if class_names is None:
        class_names = [f"class_{i}" for i in range(num_classes)]

    class_names_cn = meta.get("class_names_cn", None)
    if class_names_cn is None:
        class_names_cn = class_names

    mean = scaler.get("mean", None)
    scale = scaler.get("scale", None)

    if mean is None:
        raise ValueError("scaler.json 中没有 mean")

    if scale is None:
        raise ValueError("scaler.json 中没有 scale。注意：你的文件字段叫 scale，不是 std")

    mean = np.array(mean, dtype=np.float32).reshape(1, -1)
    scale = np.array(scale, dtype=np.float32).reshape(1, -1)

    if mean.shape[1] != input_length:
        raise ValueError(
            f"mean 维度不匹配: mean.shape={mean.shape}, input_length={input_length}"
        )

    if scale.shape[1] != input_length:
        raise ValueError(
            f"scale 维度不匹配: scale.shape={scale.shape}, input_length={input_length}"
        )

    if len(class_names) != num_classes:
        raise ValueError(
            f"class_names 数量不对: len(class_names)={len(class_names)}, num_classes={num_classes}"
        )

    if len(class_names_cn) != num_classes:
        raise ValueError(
            f"class_names_cn 数量不对: len(class_names_cn)={len(class_names_cn)}, num_classes={num_classes}"
        )

    scale = np.where(scale == 0, 1.0, scale).astype(np.float32)

    print("配置加载成功")
    print("input_length:", input_length)
    print("num_classes:", num_classes)
    print("class_names:", class_names)
    print("class_names_cn:", class_names_cn)
    print("mean shape:", mean.shape)
    print("scale shape:", scale.shape)

    return {
        "input_length": input_length,
        "num_classes": num_classes,
        "class_names": class_names,
        "class_names_cn": class_names_cn,
        "mean": mean,
        "scale": scale,
        "raw_meta": meta
    }


# =========================
# ONNX 加载
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
# 推理相关
# =========================

def normalize(x_raw, mean, scale):
    """
    标准化：
        x_norm = (x_raw - mean) / scale
    """
    return (x_raw - mean) / scale


def make_model_input(x_norm):
    """
    metadata 写的是：
        onnx_input_layout = N1L

    所以输入 ONNX 的 shape 是：
        [batch, 1, input_length]
    """
    x_norm = to_2d_float32(x_norm)
    batch_size = x_norm.shape[0]
    input_length = x_norm.shape[1]

    return x_norm.reshape(batch_size, 1, input_length).astype(np.float32)


def infer_batch(session, input_name, output_name, x_raw, cfg):
    """
    输入:
        x_raw shape = [1024] 或 [1,1024] 或 [batch,1024]

    输出:
        每个样本的分类结果
    """
    x_raw = to_2d_float32(x_raw)

    if x_raw.shape[1] != cfg["input_length"]:
        raise ValueError(
            f"输入维度错误: 当前输入 shape={x_raw.shape}, "
            f"模型需要 input_length={cfg['input_length']}"
        )

    x_norm = normalize(x_raw, cfg["mean"], cfg["scale"]).astype(np.float32)
    x_model = make_model_input(x_norm)

    logits = session.run([output_name], {input_name: x_model})[0]
    logits = np.array(logits, dtype=np.float32)

    if logits.ndim == 1:
        logits = logits.reshape(1, -1)

    if logits.shape[1] != cfg["num_classes"]:
        raise ValueError(
            f"模型输出类别数不匹配: logits.shape={logits.shape}, "
            f"num_classes={cfg['num_classes']}"
        )

    probs = softmax(logits)
    pred_ids = np.argmax(probs, axis=1)
    confidences = np.max(probs, axis=1)

    results = []

    for i in range(x_raw.shape[0]):
        pred_id = int(pred_ids[i])

        result = {
            "pred_id": pred_id,
            "class_name": cfg["class_names"][pred_id],
            "class_name_cn": cfg["class_names_cn"][pred_id],
            "confidence": float(confidences[i]),
            "probs": probs[i],
            "logits": logits[i],
            "x_raw": x_raw[i],
            "x_norm": x_norm[i],
            "x_model_shape": x_model.shape
        }

        results.append(result)

    return results


# =========================
# 随机测试
# =========================

def get_random_data(cfg, batch_size=1):
    """
    生成接近训练分布的随机原始数据。

    先生成标准正态随机数，再反标准化：
        x_raw = x_norm * scale + mean
    """
    x_norm = np.random.randn(batch_size, cfg["input_length"]).astype(np.float32)
    x_raw = x_norm * cfg["scale"] + cfg["mean"]
    return x_raw.astype(np.float32)


def random_test(args):
    cfg = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)

    print("==============================")
    print("开始随机测试")
    print("==============================")

    for i in range(args.loop):
        x_raw = get_random_data(cfg, batch_size=1)

        results = infer_batch(
            session=session,
            input_name=input_name,
            output_name=output_name,
            x_raw=x_raw,
            cfg=cfg
        )

        result = results[0]

        print("----------")
        print(f"第 {i + 1} 次随机检测")
        print("预测类别ID:", result["pred_id"])
        print("英文类别:", result["class_name"])
        print("中文类别:", result["class_name_cn"])
        print("置信度:", f"{result['confidence']:.6f}")
        print("四类概率:", np.round(result["probs"], 6))

        if args.print_data:
            print("logits:", result["logits"])
            print("模型输入shape:", result["x_model_shape"])
            print("原始输入前10维:", result["x_raw"][:10])
            print("标准化输入前10维:", result["x_norm"][:10])

        time.sleep(args.interval)


# =========================
# CSV 检测
# =========================

def read_csv_data(csv_path, input_length, has_header=False, has_label=False):
    """
    CSV格式：
        1. 每行 1024 个特征
        2. 每行 1025 列，最后一列是标签

    标签建议：
        0 = normal / 正常
        1 = slight / 轻微
        2 = moderate / 中度
        3 = severe / 重度
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
        if data.shape[1] < 2:
            raise ValueError("CSV 设置了 --has-label，但列数小于2")
        labels = data[:, -1].astype(np.int64)
        data = data[:, :-1]

    if data.shape[1] != input_length:
        raise ValueError(
            f"CSV特征维度不匹配: CSV特征列数={data.shape[1]}, "
            f"模型需要={input_length}。"
            f"如果最后一列是标签，请加 --has-label。"
        )

    return data.astype(np.float32), labels


def csv_test(args):
    cfg = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)

    data, labels = read_csv_data(
        csv_path=args.csv,
        input_length=cfg["input_length"],
        has_header=args.has_header,
        has_label=args.has_label
    )

    print("==============================")
    print("开始CSV检测")
    print("CSV路径:", args.csv)
    print("样本数量:", data.shape[0])
    print("特征维度:", data.shape[1])
    print("==============================")

    class_counts = np.zeros(cfg["num_classes"], dtype=np.int64)
    correct_count = 0
    total = data.shape[0]
    has_label = labels is not None

    batch_size = args.batch_size

    for start in range(0, total, batch_size):
        end = min(start + batch_size, total)
        batch = data[start:end]

        results = infer_batch(
            session=session,
            input_name=input_name,
            output_name=output_name,
            x_raw=batch,
            cfg=cfg
        )

        for j, result in enumerate(results):
            index = start + j
            pred_id = result["pred_id"]
            class_counts[pred_id] += 1

            line = (
                f"样本 {index + 1:04d}, "
                f"pred={pred_id}, "
                f"en={result['class_name']}, "
                f"cn={result['class_name_cn']}, "
                f"conf={result['confidence']:.6f}, "
                f"probs={np.round(result['probs'], 6)}"
            )

            if has_label:
                label = int(labels[index])
                if pred_id == label:
                    correct_count += 1
                line += f", label={label}"

            print(line)

            if args.print_data:
                print("logits:", result["logits"])
                print("模型输入shape:", result["x_model_shape"])
                print("原始输入前10维:", result["x_raw"][:10])
                print("标准化输入前10维:", result["x_norm"][:10])

    print("==============================")
    print("CSV检测完成")
    print("各类别数量:")

    for cid in range(cfg["num_classes"]):
        count = int(class_counts[cid])
        ratio = count / total if total > 0 else 0.0
        print(
            f"类别 {cid}: "
            f"{cfg['class_names'][cid]} / {cfg['class_names_cn'][cid]}, "
            f"数量={count}, "
            f"占比={ratio * 100:.2f}%"
        )

    final_pred = int(np.argmax(class_counts))

    print("------------------------------")
    print("整体多数投票结果:")
    print("类别ID:", final_pred)
    print("英文类别:", cfg["class_names"][final_pred])
    print("中文类别:", cfg["class_names_cn"][final_pred])

    if has_label:
        acc = correct_count / total if total > 0 else 0.0
        print("------------------------------")
        print("标签正确数量:", correct_count)
        print("准确率:", f"{acc * 100:.2f}%")


# =========================
# 实时接口
# =========================

def get_realtime_data(cfg):
    """
    预留真实数据接口。

    后面你接自己的实时数据时，只改这个函数。

    要求返回:
        shape = [1024]
        或 shape = [1, 1024]

    示例:
        data = read_from_dma()
        return np.array(data, dtype=np.float32).reshape(1, cfg["input_length"])

    注意:
        这里返回原始数据，不要提前标准化。
        程序会自动使用 scaler.json 里的 mean/scale 标准化。
    """

    raise NotImplementedError("请在 get_realtime_data() 中接入你的真实数据")


def realtime_test(args):
    cfg = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)

    print("==============================")
    print("开始实时检测")
    print("输入要求: 1024维原始数据")
    print("ONNX输入shape: [1, 1, 1024]")
    print("==============================")

    index = 0

    while True:
        index += 1

        x_raw = get_realtime_data(cfg)

        results = infer_batch(
            session=session,
            input_name=input_name,
            output_name=output_name,
            x_raw=x_raw,
            cfg=cfg
        )

        result = results[0]

        print(
            f"第 {index} 次实时检测: "
            f"pred={result['pred_id']}, "
            f"en={result['class_name']}, "
            f"cn={result['class_name_cn']}, "
            f"conf={result['confidence']:.6f}, "
            f"probs={np.round(result['probs'], 6)}"
        )

        time.sleep(args.interval)




# =========================
# C端二进制数据读取与结果写入
# =========================

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


def read_bin_window_1ch(bin_path, seq_len, max_ch=8, max_points=4096, channel=3, offset=0):
    """读取 C 端 int new_data[MAX_CH][MAX_POINTS] 中指定单通道窗口。"""
    raw = np.fromfile(bin_path, dtype=np.int32)
    expected_size = max_ch * max_points
    if raw.size != expected_size:
        raise ValueError(f"二进制文件大小不匹配: 期望 {expected_size} 个int32, 实际 {raw.size}")
    data = raw.reshape(max_ch, max_points)
    if channel < 0 or channel >= max_ch:
        raise ValueError(f"channel={channel} 超出范围 [0, {max_ch - 1}]")
    if offset + seq_len > max_points:
        raise ValueError(f"offset({offset}) + seq_len({seq_len}) 超出 max_points({max_points})")
    return data[channel, offset:offset + seq_len].astype(np.float32)


def read_bin_window_8ch32(bin_path, max_ch=8, max_points=4096, offset=0, window_size=32):
    """读取 C 端数据中 8 通道 x 32 点窗口，返回 shape=[8,32]。"""
    raw = np.fromfile(bin_path, dtype=np.int32)
    expected_size = max_ch * max_points
    if raw.size != expected_size:
        raise ValueError(f"二进制文件大小不匹配: 期望 {expected_size} 个int32, 实际 {raw.size}")
    data = raw.reshape(max_ch, max_points)
    if max_ch < 8:
        raise ValueError(f"max_ch={max_ch} 小于8，无法读取8通道窗口")
    if offset + window_size > max_points:
        raise ValueError(f"offset({offset}) + window_size({window_size}) 超出 max_points({max_points})")
    return data[:8, offset:offset + window_size].astype(np.float32)


# =========================
# 连续检测 watch 模式
# =========================

def watch_test(args):
    cfg = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model)
    input_length = cfg["input_length"]

    print("========== 连续检测 watch 模式 ==========")
    print("数据文件:", args.data)
    print("触发文件:", args.trigger)
    print("停止文件:", args.stop_file)
    print("结果文件:", args.result_file)
    print("algo_id:", args.algo_id)
    print("algo_type:", args.algo_type)
    print("input_length:", input_length)
    print("channel:", args.channel)
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
                seq = read_trigger_seq(args.trigger)
                try:
                    x_raw = read_bin_window_1ch(args.data, input_length, args.max_ch, args.max_points, args.channel, args.offset)
                    results = infer_batch(session, input_name, output_name, x_raw, cfg)
                    result = results[0]
                    result_code = int(result["pred_id"])
                    index += 1
                    print(f"[watch #{index}] pred={result_code}, cn={result['class_name_cn']}, conf={result['confidence']:.6f}, seq={seq}")
                    write_watch_result(args.result_file, args.algo_id, args.algo_type, result_code, seq)
                except Exception as e:
                    print(f"[watch] 检测失败: {e}")
        time.sleep(args.interval)

# =========================
# 主函数
# =========================

def parse_args():
    parser = argparse.ArgumentParser(
        description="1024维Conv1D四分类ONNX检测程序，输入shape为[batch,1,1024]"
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
        "--scaler",
        type=str,
        default=DEFAULT_SCALER_PATH,
        help="scaler.json路径"
    )

    parser.add_argument(
        "--csv",
        type=str,
        default=DEFAULT_CSV_PATH,
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
        help="CSV最后一列是否为标签"
    )

    parser.add_argument(
        "--batch-size",
        type=int,
        default=1,
        help="CSV检测时每批推理多少个样本"
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
        help="是否打印输入、标准化输入和logits"
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
        default=DEFAULT_RESULT_FILE,
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

    args, unknown_args = parser.parse_known_args()
    if unknown_args:
        for item in unknown_args:
            if (
                args.result_file == DEFAULT_RESULT_FILE
                and not item.startswith("-")
                and os.path.isabs(item)
                and ("result" in os.path.basename(item) or item.endswith(".stream"))
            ):
                args.result_file = item
                break
        print("忽略未识别参数:", unknown_args)

    return args


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
