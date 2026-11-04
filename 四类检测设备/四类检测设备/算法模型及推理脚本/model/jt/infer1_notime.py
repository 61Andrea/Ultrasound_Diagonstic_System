import os
import json
import numpy as np
import onnxruntime as ort


# ============================================================
# 1. 路径配置
# ============================================================

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

MODEL_DIR = os.path.join(BASE_DIR, "model")
DATA_DIR = os.path.join(BASE_DIR, "data")

ONNX_PATH = os.path.join(MODEL_DIR, "sae_model.onnx")
SCALER_PATH = os.path.join(MODEL_DIR, "scaler.json")
META_PATH = os.path.join(MODEL_DIR, "metadata.json")

DATA_PATH = os.path.join(DATA_DIR, "jt.txt")


# ============================================================
# 2. 文件检查
# ============================================================

def check_file_exists(path, name):
    if not os.path.exists(path):
        raise FileNotFoundError("{} 不存在: {}".format(name, path))


check_file_exists(ONNX_PATH, "ONNX模型文件")
check_file_exists(SCALER_PATH, "scaler.json")
check_file_exists(META_PATH, "metadata.json")
check_file_exists(DATA_PATH, "数据文件")


# ============================================================
# 3. 读取纯数字 txt
# ============================================================

def read_plain_feature_txt(file_path):
    """
    读取纯数字 txt。

    支持两种格式：

    1）一行 256 个数：
       1.0 2.0 3.0 ... 256.0

    2）多行，每行 256 个数：
       1.0 2.0 ... 256.0
       1.1 2.1 ... 256.1
       ...

    返回形状：
       单条样本: 1 x 256
       多条样本: N x 256
    """

    x = np.loadtxt(file_path, dtype=np.float32)

    # 如果只有一行，np.loadtxt 读出来是一维，需要转成 1 x 256
    if x.ndim == 1:
        x = x.reshape(1, -1)

    return x.astype(np.float32)


# ============================================================
# 4. 加载 scaler.json
# ============================================================

with open(SCALER_PATH, "r", encoding="utf-8") as f:
    scaler_data = json.load(f)

mean = np.array(scaler_data["mean"], dtype=np.float32)
scale = np.array(scaler_data["scale"], dtype=np.float32)

print("scaler 特征维度:", mean.shape[0])


# ============================================================
# 5. 加载 metadata.json，读取阈值
# ============================================================

with open(META_PATH, "r", encoding="utf-8") as f:
    metadata = json.load(f)

threshold = metadata.get("threshold", None)

if threshold is None:
    threshold = metadata.get("default_threshold", None)

if threshold is None:
    raise ValueError("metadata.json 里没有 threshold 或 default_threshold")

threshold = float(threshold)

print("threshold:", threshold)


# ============================================================
# 6. 加载 ONNX 模型
# ============================================================

session = ort.InferenceSession(
    ONNX_PATH,
    providers=["CPUExecutionProvider"]
)

input_name = session.get_inputs()[0].name
output_name = session.get_outputs()[0].name

print("ONNX 输入名:", input_name)
print("ONNX 输出名:", output_name)


# ============================================================
# 7. 读取真实 feature.txt
# ============================================================

x = read_plain_feature_txt(DATA_PATH)

print("读取到样本数:", x.shape[0])
print("输入维度:", x.shape)
print("第一条数据前5个值:", x[0, :5])


# ============================================================
# 8. 检查维度
# ============================================================

if x.ndim != 2:
    raise ValueError("输入数据必须是二维矩阵，当前维度: {}".format(x.shape))

if x.shape[1] != mean.shape[0]:
    raise ValueError(
        "输入特征维度不对，当前是 {} 维，但 scaler 需要 {} 维".format(
            x.shape[1], mean.shape[0]
        )
    )

if x.shape[1] != 256:
    raise ValueError(
        "模型要求输入 256 维，当前输入是 {} 维".format(x.shape[1])
    )


# ============================================================
# 9. 标准化
# ============================================================

# 等价于 sklearn 的 scaler.transform(x)
x_scaled = (x - mean) / scale
x_scaled = x_scaled.astype(np.float32)


# ============================================================
# 10. ONNX 推理
# ============================================================

y_recon = session.run(
    [output_name],
    {input_name: x_scaled}
)[0]


# ============================================================
# 11. 计算重构误差
# ============================================================

errors = np.mean(np.square(x_scaled - y_recon), axis=1)


# ============================================================
# 12. 判断正常 / 异常
# ============================================================

preds = (errors > threshold).astype(np.int32)


# ============================================================
# 13. 输出结果
# ============================================================

normal_count = 0
abnormal_count = 0

for i in range(len(preds)):
    result = "异常" if preds[i] == 1 else "正常"

    if preds[i] == 1:
        abnormal_count += 1
    else:
        normal_count += 1

    print("第{}条: error={:.6f}, prediction={}, result={}".format(
        i,
        float(errors[i]),
        int(preds[i]),
        result
    ))


print("========== 汇总 ==========")
print("总样本数:", len(preds))
print("正常数量:", normal_count)
print("异常数量:", abnormal_count)