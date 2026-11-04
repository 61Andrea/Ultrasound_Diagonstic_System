# 多尺度 CNN 腐蚀诊断 — ONNX 部署说明（小白版 / ZU5EV PS 端）

这份说明和 `jiance` 里的「稀疏自编码器 + ONNX」是**同一套思路**，只是模型换成了**多尺度 1D-CNN 做 4 类腐蚀程度分类**（无腐蚀 / 轻度 / 中度 / 重度）。

---

## 一、整体你在做什么

| 步骤 | 在电脑上（训练） | 在 ZU5EV 板子 PS 上（推理） |
|------|------------------|-----------------------------|
| 作用 | 用 `1.txt`～`16.txt` 训好网络，并导出 ONNX | 只装轻量依赖，用 ONNX Runtime 跑诊断 |
| 脚本 | `mscnn_onnx.py` | `mscnn_onnxruntime_inference.py` |
| 原始训练脚本 | `data_preprocessing_multiscale_cnn.py`（逻辑一致，本目录已保留） | 板子上**不需要**安装 PyTorch |

板子 **PS** 一般指 ARM + Linux（如 PetaLinux），能跑 Python 即可；推理用 **CPU** 即可（脚本里已写 `CPUExecutionProvider`）。

---

## 二、训练完成后，你要拷贝哪些文件

**结论先说：** 拷到 PS 上做推理，**一般只要 `model/` 里那三样 + 推理脚本即可**，**不需要**把 `data/` 里 `1.txt`～`16.txt` 等训练数据拷到板子上。训练数据只在 PC 上用来学模型；板子上跑的是**现场采集或你另存的测试数据**，格式对（256 维 + 同一套 scaler 对应的 ONNX）就行。

下面分两块看目录，避免和「整仓工程」混在一起。

### 2.1 本机上的完整工程（训练 + 导出，可选保留）

与 `jiance` 一样，建议在 PC 上固定一个 `model` 文件夹（文件名与 SAE 不同，避免混用）：

```text
zhenduan/   （本机）
├── mscnn_onnx.py
├── mscnn_onnxruntime_inference.py
├── data_preprocessing_multiscale_cnn.py   # 想重新训练时可留
├── data/                    # 1.txt～16.txt，仅训练阶段需要
└── model/
    ├── mscnn_model.onnx
    ├── mscnn_scaler.pkl
    ├── mscnn_metadata.json
    ├── mscnn_weights.pth     # 可选，备份/再导出 ONNX
    └── mscnn_model_info.pkl  # 可选
```

### 2.2 拷到 ZU5EV PS 上的最小集合（推理）

```text
mscnn_deploy/   （板子上的目录名随意）
├── mscnn_onnxruntime_inference.py
└── model/
    ├── mscnn_model.onnx
    ├── mscnn_scaler.pkl
    └── mscnn_metadata.json
```

**不必拷到 PS 的（除非你要在板上用同一批 txt 做离线对拍）：**

- `data/`、`1.txt`～`16.txt`（训练数据）
- `mscnn_onnx.py`、`data_preprocessing_multiscale_cnn.py`、PyTorch 权重等（板子不训练就不需要）

**可选拷：** `mscnn_weights.pth`、`mscnn_model_info.pkl` —— 仅当你打算在板子以外的地方**再导出 ONNX** 或排查问题时有用；纯推理不需要。

## 三、各文件是干什么的（用人话）

1. **`mscnn_model.onnx`**  
   已经固化好的 CNN，输入一段 256 维特征（会先按训练时同样方式标准化），输出 4 类分数（logits）。

2. **`mscnn_scaler.pkl`**  
   训练时 `StandardScaler` 拟合结果。现场数据**必须先按同样 scaler 变换**，否则结果不可信。

3. **`mscnn_metadata.json`**  
   记录 `feature_dimension`（默认 256）、`class_names`（四类中文名）、类别数等，推理脚本用来校验维度和写结果。

4. **`mscnn_onnxruntime_inference.py`**  
   读数据 → 标准化 → ONNX 推理 → `argmax` 得到类别 → 写结果到 `test_results/`。

---

## 四、在 Windows / 电脑上：怎么训练并导出 ONNX

### 1）安装依赖（训练机）

```bash
pip install torch numpy pandas scikit-learn scipy matplotlib seaborn joblib onnx
```

（若只做**已有权重**的导出，可只装 `torch joblib onnx`。）

### 2）准备数据

与 `data_preprocessing_multiscale_cnn.py` 相同：在 `--data_dir` 下放 `1.txt`～`16.txt`，每行制表符分隔，**第 1 列为占位/序号，第 2～257 列为 256 维特征**（与原始脚本一致）。

本仓库示例数据目录可为：

```text
zhenduan/data/
```

### 3）一条命令：训练 + 导出 ONNX

在 **`zhenduan` 目录下**打开终端：

```bash
cd "d:\Cursor code\silei_4.3\fushi\zhenduan"
python mscnn_onnx.py --train --data_dir "d:\Cursor code\silei_4.3\fushi\zhenduan\data"
```

成功后 `model/` 下会有 `mscnn_model.onnx`、`mscnn_scaler.pkl`、`mscnn_metadata.json` 等。

### 4）如果你以前已经用旧脚本训好了，只想补导出 ONNX

把 `mscnn_weights.pth`、`mscnn_scaler.pkl`、`mscnn_model_info.pkl` 放进 `model/`，然后：

```bash
python mscnn_onnx.py --export-only --model_dir model
```

---

## 五、在电脑上先跑通推理（再上板）

### 1）安装推理依赖

```bash
pip install onnxruntime numpy joblib
```

若要用 **带标签 `.mat` 评估**，再装 `scipy`；若要用脚本里打印的 sklearn 指标，再装 `scikit-learn`：

```bash
pip install scipy scikit-learn
```

### 2）数据格式（与 `data_preprocessing_multiscale_cnn.py` 一致）

| 场景 | 格式说明 |
|------|-----------|
| **训练同款 txt**（如 `data/1.txt`） | 每行 **制表符**分隔：第 1 列为序号/占位，第 2～257 列为 **256 维特征**（与训练里 `data_parts[1:257]` 一致）。 |
| **实时采集 / 无标签** | 每行 **仅 256 个数值**，分隔符为 **制表符或逗号**；无首列占位。 |
| **表格文件** | `.csv` 默认按「纯 256 列」读取；`.txt` 可用 `--input_format auto` 根据首行自动区分上两种（也可强制 `training_txt` / `pure256`）。 |

### 3）批处理推理（文件）

与训练数据相同的 `1.txt`：

```bash
python mscnn_onnxruntime_inference.py --input_path "./data/1.txt"
```

（未写 `--input_path` 时默认尝试 `./data/1.txt`。）

纯 256 列 `csv` / `npy`：

```bash
python mscnn_onnxruntime_inference.py --input_path "你的路径\features.csv" --input_format pure256
```

结果默认在 `test_results/mscnn_predictions_no_label.txt`。终端会打印类别分布与前 `--preview` 条预览。

### 4）标准输入实时流（采集进程管道对接）

模型加载信息在 **stderr**，**stdout 每行一条预测**，便于 `your_acquisition | python3 ...`：

```bash
python3 mscnn_onnxruntime_inference.py --mode stdin --stdin_layout pure256
```

- `pure256`：每行 256 个采集点（逗号或制表符）。
- `training_txt`：每行与训练 txt 相同（首列 + 256 维）。

每行输出：`预测类别索引 \t 中文类名 \t max_logit`。

### 5）在程序里调用（推荐接口，无标签实时诊断）

```python
from mscnn_onnxruntime_inference import create_classifier
import numpy as np

clf = create_classifier("model")  # 或 MSCNNOnnxRuntimeClassifier(Path("model")); clf.load_model()

# 单次采集：shape (256,) 或 (1, 256)
pred_label, logits = clf.diagnose_one(acquisition_buffer, verbose=False)

# 缓冲多帧再算：shape (N, 256)
pred_labels, logits = clf.diagnose_batch(feature_matrix, verbose=False)
```

也可用 `parse_training_txt_line(line)` / `load_training_format_txt(path)` 先得到 `numpy` 再送入 `diagnose_*`。

### 6）带标签的 `.mat`（可选，仅用于离线算准确率）

在 MATLAB 或 Python 里准备一个变量 **`labeled_data`**，形状 `(样本数, 257)`：

- 第 **1** 列：真实类别 **0～3**（与训练时四类顺序一致）
- 第 **2～257** 列：256 维特征

```bash
python mscnn_onnxruntime_inference.py --input_type mat --input_path "D:\你的路径\test_labeled.mat"
```

结果在 `test_results/mscnn_predictions.txt`。此路径会 **惰性导入 scipy**；若板子完全不装 scipy，请不要使用 `mat` 模式。

### 7）模型不在当前目录时

```bash
python mscnn_onnxruntime_inference.py --model_dir "D:\...\zhenduan\model" --input_path "D:\...\data\1.txt"
```

路径含空格时，整条路径用**英文双引号**包起来。板子 **只做实时推理、不用 mat** 时，最小依赖可为：`onnxruntime`、`numpy`、`joblib`。

## 六、拷到 ZU5EV PS（Linux）上怎么做

### 1）把文件拷到板子

可用 U 盘、`scp`、NFS 等，例如：

```bash
scp mscnn_onnxruntime_inference.py root@<板子IP>:/home/root/mscnn_deploy/
scp -r model root@<板子IP>:/home/root/mscnn_deploy/
```

### 2）在板子上安装 Python 与 onnxruntime

具体命令随你的根文件系统/包管理器略有不同，思路是：

```bash
python3 --version
pip3 install onnxruntime numpy joblib
# 若需 mat 评估再加: pip3 install scipy scikit-learn
```

若 `pip install onnxruntime` 失败（ARM 上偶发），可：

- 换用板厂或社区提供的 **aarch64** 预编译 wheel；
- 或在 PC 上针对 `manylinux_aarch64` / 板子 glibc 版本下载对应 wheel 再 `pip install xxx.whl`。

**不需要**在板子上安装 PyTorch。

### 3）在板子上运行

```bash
cd /home/root/mscnn_deploy
# 批处理：训练同款 txt
python3 mscnn_onnxruntime_inference.py --input_path ./data/1.txt
# 或实时管道（采集写 stdout，每行 256 个数）
your_sensor_reader | python3 mscnn_onnxruntime_inference.py --mode stdin --stdin_layout pure256
```

### 4）部署后怎么验收（测试数据要放吗、放哪）

**要放一份测试数据**，用来确认「模型能加载、能跑完、能出预测」；**放哪都行**，只要命令里 **`--input_path` 写对路径**。习惯上在部署目录下建子文件夹即可，例如：

```text
mscnn_deploy/
├── mscnn_onnxruntime_inference.py
├── model/
│   └── ...
└── test_data/          # 自建，名字随意
    └── sample.txt      # 下面任选一种内容拷进来
```

**推荐用哪一份数据（三选一即可）：**

| 方式 | 说明 | 命令要点 |
|------|------|----------|
| **A. 训练同款 txt（最直观）** | 从 PC 拷 **`data/1.txt`** 全文件或**前几行**到板子 `test_data/`，格式与训练一致（首列 + 256 维）。 | `python3 mscnn_onnxruntime_inference.py --input_path ./test_data/1_head.txt` |
| **B. 纯 256 列 csv** | PC 上若已有抽样 csv（如 `21.csv`，无表头、每行 256 个数），拷到板子。 | 加 `--input_format pure256`（或 `.csv` 且 `auto` 一般即可） |
| **C. 只测链路** | 任意与训练**同量纲**的 256 维样本若干行；不要求标签。 | 能跑通即说明部署成功；类别是否合理需你心里有数的样本对照 |

**不必**为了验收把整套 `1.txt`～`16.txt` 都拷上去；**几条或一个小文件**就够。

验收成功时你会看到：终端里类别分布/预览（批处理模式），或 `test_results/mscnn_predictions_no_label.txt` 里逐条预测。

---

## 七、和 `jiance` 里 SAE 部署的对应关系（方便你记）

| 项目 | SAE（`jiance`） | 多尺度 CNN（`zhenduan`） |
|------|-----------------|---------------------------|
| 训练+导出 | `sae_onnx.py` | `mscnn_onnx.py` |
| 推理 | `onnxruntime_inference.py` | `mscnn_onnxruntime_inference.py` |
| ONNX 文件名 | `sae_model.onnx` | `mscnn_model.onnx` |
| 标准化器 | `scaler.pkl` | `mscnn_scaler.pkl` |
| 元数据 | `metadata.json` | `mscnn_metadata.json` |
| 任务 | 正常/异常二分类（重构误差） | 4 类腐蚀程度（argmax） |

---

## 八、小白最容易踩的坑

1. **没训练就推理**  
   `model/mscnn_model.onnx` 不存在会报错。请先 `--train` 或 `--export-only`。

2. **特征维数不是 256**  
   与训练时 `feature_dimension` 不一致会在预处理或 ONNX 里报错。

3. **Scaler 与模型不是同一套**  
   必须用本次训练导出的 `mscnn_scaler.pkl`，不能混用别的实验的 scaler。

4. **`.mat` 变量名写错**  
   推理脚本只认变量名 **`labeled_data`**（与本文约定一致）。

5. **在板子上装错 onnxruntime 架构**  
   用 `uname -m` 看是 `aarch64` 还是 `armv7l`，选对应 wheel/包。

---

## 九、最短命令备忘

**PC 上训练并导出：**

```bash
python mscnn_onnx.py --train --data_dir "./data"
```

**PC 或板子上推理（训练同款 `data/1.txt`）：**

```bash
python mscnn_onnxruntime_inference.py --input_path "./data/1.txt"
```

**实时标准输入（每行 256 维）：**

```bash
python mscnn_onnxruntime_inference.py --mode stdin --stdin_layout pure256
```

---

## 十、数据来源说明（与原始脚本一致）

`mscnn_onnx.py` 仍按 `data_preprocessing_multiscale_cnn.py` 的规则：  
`1.txt`～`4.txt` → 轻度，`5`～`8` → 中度，`9`～`12` → 重度，`13`～`16` → 无腐蚀。

批处理读入时：与训练完全相同的行格式见上文「训练同款 txt」；**仅 256 维、无首列**的现场数据请使用 `--input_format pure256` 或 `--mode stdin --stdin_layout pure256`。在采集线程里请直接调用 `diagnose_one` / `diagnose_batch`（见第五节）。
