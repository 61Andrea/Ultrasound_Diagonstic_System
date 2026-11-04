import os
import json
import time
import argparse
import numpy as np
import onnxruntime as ort


# =========================
# 默认路径
# =========================

DEFAULT_MODEL_PATH = "/home/root/model/mscnn_model.onnx"
DEFAULT_METADATA_PATH = "/home/root/model/mscnn_metadata.json"
DEFAULT_SCALER_PATH = "/home/root/model/scaler.json"
DEFAULT_CSV_PATH = "/home/root/model/test_data.csv"


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
        raise ValueError(f"数组维度错误，期望二维数组，实际 shape={arr.shape}")

    return arr


# =========================
# 加载配置
# =========================

def load_config(metadata_path, scaler_path):
    meta = load_json(metadata_path)

    if os.path.exists(scaler_path):
        scaler = load_json(scaler_path)
    else:
        raise FileNotFoundError(
            f"没有找到 scaler.json: {scaler_path}。"
            f"这个模型需要 scaler.json 里的 mean 和 scale。"
        )

    feature_dim = get_value_from_dict(
        meta,
        ["feature_dimension", "feature_dim", "input_dim", "dim"],
        default=None
    )

    num_classes = get_value_from_dict(
        meta,
        ["num_classes", "n_classes"],
        default=None
    )

    class_names = meta.get("class_names", None)

    input_name_from_meta = meta.get("onnx_input_name", None)
    output_name_from_meta = meta.get("onnx_output_name", None)

    mean = get_value_from_dict(
        scaler,
        ["mean", "mean_", "scaler_mean"],
        default=None
    )

    scale = get_value_from_dict(
        scaler,
        ["scale", "scale_", "std", "std_", "scaler_std"],
        default=None
    )

    if feature_dim is None:
        if mean is not None:
            feature_dim = len(mean)
        elif scale is not None:
            feature_dim = len(scale)
        else:
            raise ValueError("无法确定 feature_dimension，请检查 metadata 或 scaler")

    feature_dim = int(feature_dim)

    if num_classes is None:
        raise ValueError("没有找到 num_classes，请检查 metadata")

    num_classes = int(num_classes)

    if class_names is None:
        class_names = [f"class_{i}" for i in range(num_classes)]

    if len(class_names) != num_classes:
        raise ValueError(
            f"class_names 数量和 num_classes 不一致: "
            f"len(class_names)={len(class_names)}, num_classes={num_classes}"
        )

    if mean is None:
        raise ValueError("scaler.json 中没有 mean")

    if scale is None:
        raise ValueError("scaler.json 中没有 scale/std")

    mean = np.array(mean, dtype=np.float32).reshape(1, -1)
    scale = np.array(scale, dtype=np.float32).reshape(1, -1)

    if mean.shape[1] != feature_dim:
        raise ValueError(
            f"mean 维度不匹配: mean.shape={mean.shape}, feature_dim={feature_dim}"
        )

    if scale.shape[1] != feature_dim:
        raise ValueError(
            f"scale 维度不匹配: scale.shape={scale.shape}, feature_dim={feature_dim}"
        )

    scale = np.where(scale == 0, 1.0, scale).astype(np.float32)

    print("配置加载成功")
    print("feature_dim:", feature_dim)
    print("num_classes:", num_classes)
    print("class_names:", class_names)
    print("mean shape:", mean.shape)
    print("scale shape:", scale.shape)
    print("metadata input name:", input_name_from_meta)
    print("metadata output name:", output_name_from_meta)

    return {
        "feature_dim": feature_dim,
        "num_classes": num_classes,
        "class_names": class_names,
        "mean": mean,
        "scale": scale,
        "input_name_from_meta": input_name_from_meta,
        "output_name_from_meta": output_name_from_meta,
        "raw_meta": meta
    }


# =========================
# ONNX 模型加载
# =========================

def create_onnx_session(model_path, cfg):
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
    print("ONNX输入名:", input_name)
    print("ONNX输入shape:", input_info.shape)
    print("ONNX输出名:", output_name)
    print("ONNX输出shape:", output_info.shape)

    # 如果 metadata 里指定了名字，优先提示是否一致
    if cfg["input_name_from_meta"] is not None and cfg["input_name_from_meta"] != input_name:
        print(
            "警告: metadata 中的输入名和ONNX实际输入名不一致，"
            f"metadata={cfg['input_name_from_meta']}, onnx={input_name}。"
            "程序将使用ONNX实际输入名。"
        )

    if cfg["output_name_from_meta"] is not None and cfg["output_name_from_meta"] != output_name:
        print(
            "警告: metadata 中的输出名和ONNX实际输出名不一致，"
            f"metadata={cfg['output_name_from_meta']}, onnx={output_name}。"
            "程序将使用ONNX实际输出名。"
        )

    return session, input_name, output_name


# =========================
# 数据预处理与推理
# =========================

def normalize_features(x_raw, mean, scale):
    """
    标准化：
        x_norm = (x_raw - mean) / scale

    输入 x_raw shape:
        [1, 256] 或 [batch, 256]

    输出:
        [1, 256] 或 [batch, 256]
    """
    return (x_raw - mean) / scale


def make_model_input(x_norm):
    """
    这个模型 metadata 写的是：
        input_shape_note = (batch, 1, feature_dim)

    所以输入要整理成：
        [batch, 1, 256]
    """
    x_norm = to_2d_float32(x_norm)
    batch_size = x_norm.shape[0]
    feature_dim = x_norm.shape[1]

    x_model = x_norm.reshape(batch_size, 1, feature_dim).astype(np.float32)

    return x_model


def infer_once(session, input_name, output_name, x_raw, cfg):
    """
    单个样本或多个样本分类。

    输入:
        x_raw shape = [256] 或 [1,256] 或 [batch,256]

    流程:
        原始特征
        -> 标准化
        -> reshape [batch,1,256]
        -> ONNX推理
        -> softmax
        -> argmax类别
    """

    x_raw = to_2d_float32(x_raw)

    if x_raw.shape[1] != cfg["feature_dim"]:
        raise ValueError(
            f"输入维度错误: 当前输入 shape={x_raw.shape}, "
            f"模型需要 feature_dim={cfg['feature_dim']}"
        )

    x_norm = normalize_features(
        x_raw,
        cfg["mean"],
        cfg["scale"]
    ).astype(np.float32)

    x_model = make_model_input(x_norm)

    logits = session.run([output_name], {input_name: x_model})[0]
    logits = np.array(logits, dtype=np.float32)

    if logits.ndim == 1:
        logits = logits.reshape(1, -1)

    if logits.shape[1] != cfg["num_classes"]:
        raise ValueError(
            f"模型输出类别数不匹配: logits.shape={logits.shape}, "
            f"metadata num_classes={cfg['num_classes']}"
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

    先生成标准正态 x_norm，
    再反标准化：
        x_raw = x_norm * scale + mean
    """

    x_norm = np.random.randn(batch_size, cfg["feature_dim"]).astype(np.float32)
    x_raw = x_norm * cfg["scale"] + cfg["mean"]
    return x_raw.astype(np.float32)


def random_test(args):
    cfg = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model, cfg)

    print("==============================")
    print("开始随机数据测试")
    print("==============================")

    for i in range(args.loop):
        x_raw = get_random_data(cfg, batch_size=1)

        results = infer_once(
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
        print("预测类别:", result["class_name"])
        print("置信度:", f"{result['confidence']:.6f}")
        print("四类概率:", np.round(result["probs"], 6))

        if args.print_data:
            print("logits:", result["logits"])
            print("模型输入shape:", result["x_model_shape"])
            print("原始输入前10维:", result["x_raw"][:10])
            print("标准化输入前10维:", result["x_norm"][:10])

        time.sleep(args.interval)


# =========================
# CSV读取
# =========================

def read_csv_data(csv_path, feature_dim, has_header=False, has_label=False):
    """
    读取CSV。

    支持：
        1. 每行 256 个特征
        2. 每行 257 列，最后一列是标签

    标签格式：
        0 = 无腐蚀
        1 = 轻度腐蚀
        2 = 中度腐蚀
        3 = 重度腐蚀
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
            raise ValueError("CSV设置了 has_label，但列数小于2")
        labels = data[:, -1].astype(np.int64)
        data = data[:, :-1]

    if data.shape[1] != feature_dim:
        raise ValueError(
            f"CSV特征维度不匹配: CSV特征列数={data.shape[1]}, "
            f"模型需要={feature_dim}。"
            f"如果最后一列是标签，请加 --has-label。"
        )

    return data.astype(np.float32), labels


def csv_test(args):
    cfg = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model, cfg)

    data, labels = read_csv_data(
        csv_path=args.csv,
        feature_dim=cfg["feature_dim"],
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
    has_label = labels is not None

    batch_size = args.batch_size
    total = data.shape[0]

    for start in range(0, total, batch_size):
        end = min(start + batch_size, total)
        batch = data[start:end]

        results = infer_once(
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
                f"class={result['class_name']}, "
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
            f"类别 {cid}: {cfg['class_names'][cid]}, "
            f"数量={count}, "
            f"占比={ratio * 100:.2f}%"
        )

    final_pred = int(np.argmax(class_counts))
    final_class = cfg["class_names"][final_pred]

    print("------------------------------")
    print("整体多数投票结果:")
    print("类别ID:", final_pred)
    print("类别名称:", final_class)

    if has_label:
        acc = correct_count / total if total > 0 else 0.0
        print("------------------------------")
        print("标签正确数量:", correct_count)
        print("准确率:", f"{acc * 100:.2f}%")


# =========================
# 实时数据接口
# =========================

def get_realtime_data(cfg):
    """
    预留真实数据接口。

    后面你接自己的数据时，只需要改这个函数。

    要求返回：
        shape = [256]
        或 shape = [1,256]

    示例：
        data = read_from_dma()
        return np.array(data, dtype=np.float32).reshape(1, cfg["feature_dim"])

    注意：
        这里返回原始特征，不要提前标准化。
        程序会自动使用 scaler.json 里的 mean/scale 标准化。
    """

    raise NotImplementedError("请在 get_realtime_data() 中接入你的真实数据")


def realtime_test(args):
    cfg = load_config(args.metadata, args.scaler)
    session, input_name, output_name = create_onnx_session(args.model, cfg)

    print("==============================")
    print("开始实时检测")
    print("输入要求: 256维原始特征")
    print("==============================")

    index = 0

    while True:
        index += 1

        x_raw = get_realtime_data(cfg)

        results = infer_once(
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
            f"class={result['class_name']}, "
            f"conf={result['confidence']:.6f}, "
            f"probs={np.round(result['probs'], 6)}"
        )

        time.sleep(args.interval)


# =========================
# 主函数
# =========================

def parse_args():
    parser = argparse.ArgumentParser(
        description="MultiScaleCNN 256维4分类ONNX检测程序"
    )

    parser.add_argument(
        "--mode",
        type=str,
        default="random",
        choices=["random", "csv", "realtime"],
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
        help="mscnn_metadata.json路径"
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

    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()

    if args.mode == "random":
        random_test(args)

    elif args.mode == "csv":
        csv_test(args)

    elif args.mode == "realtime":
        realtime_test(args)