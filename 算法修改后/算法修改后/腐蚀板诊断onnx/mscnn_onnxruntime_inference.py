"""
多尺度 CNN 腐蚀诊断 — ONNX Runtime 推理（面向 ZU5EV PS / 实时采集）。

数据约定
--------
- **训练用 txt**：每行制表符 ``\\t`` 分隔，第 1 列为序号/占位，其余为特征维。
- **实时采集**：每行仅特征数值（无标签），分隔符可为制表符或逗号。

对外稳定接口（嵌入你自己的采集线程时优先用这些）
-----------------------------------------------
- ``MSCNNOnnxRuntimeClassifier.load_model()``：加载 onnx / scaler / metadata。
- ``diagnose_batch(features, verbose=False)``：``features`` 形状 ``(N, 256)`` → ``(pred_labels, logits)``。
- ``diagnose_one(sample_256, verbose=False)``：单条 ``(256,)`` 或 ``(1,256)`` → ``(pred_label: int, logits: (num_classes,))``。
- ``parse_training_txt_line(line)`` / ``load_training_format_txt(path)``：与训练数据相同的行格式。
- ``create_classifier(model_dir)``：一步 ``load_model`` 并返回实例。

命令行仍支持批量文件；``.mat`` 仅作可选评估，且 **惰性导入 scipy**，不跑 mat 时 PS 上可不装 scipy。
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Literal, Tuple

import joblib
import numpy as np
import onnxruntime as ort


DEFAULT_MODEL_DIR = Path("model")
DEFAULT_OUTPUT_DIR = Path("test_results")
_DEFAULT_ZD = Path(__file__).resolve().parent
# 与训练数据目录一致时可用 data/1.txt；不存在则请在命令行指定 --input_path
DEFAULT_FEATURE_FILE = _DEFAULT_ZD / "data" / "1.txt"

ONNX_NAME = "mscnn_model.onnx"
SCALER_NAME = "mscnn_scaler.pkl"
METADATA_NAME = "mscnn_metadata.json"

InputFileFormat = Literal["auto", "pure256", "training_txt"]
RunMode = Literal["file", "stdin"]


__all__ = [
    "MSCNNOnnxRuntimeClassifier",
    "create_classifier",
    "DEFAULT_MODEL_DIR",
    "DEFAULT_OUTPUT_DIR",
    "DEFAULT_FEATURE_FILE",
]


def create_classifier(model_dir: str | Path = DEFAULT_MODEL_DIR) -> "MSCNNOnnxRuntimeClassifier":
    """构造并 ``load_model()``，便于在其它模块中一行接入。"""
    clf = MSCNNOnnxRuntimeClassifier(Path(model_dir))
    clf.load_model()
    return clf


class MSCNNOnnxRuntimeClassifier:
    def __init__(self, model_dir: Path = DEFAULT_MODEL_DIR):
        self.model_dir = Path(model_dir)
        self.onnx_path = self.model_dir / ONNX_NAME
        self.scaler_path = self.model_dir / SCALER_NAME
        self.metadata_path = self.model_dir / METADATA_NAME

        self.session: ort.InferenceSession | None = None
        self.input_name: str | None = None
        self.output_name: str | None = None
        self.scaler = None
        self.metadata: dict | None = None
        self.feature_dim: int | None = None
        self.class_names: list[str] = []

    def load_model(self) -> None:
        if not self.onnx_path.exists():
            raise FileNotFoundError(f"找不到 ONNX: {self.onnx_path}")
        if not self.scaler_path.exists():
            raise FileNotFoundError(f"找不到标准化器: {self.scaler_path}")
        if not self.metadata_path.exists():
            raise FileNotFoundError(f"找不到元数据: {self.metadata_path}")

        self.session = ort.InferenceSession(str(self.onnx_path), providers=["CPUExecutionProvider"])
        self.input_name = self.session.get_inputs()[0].name
        self.output_name = self.session.get_outputs()[0].name
        self.scaler = joblib.load(self.scaler_path)

        with open(self.metadata_path, "r", encoding="utf-8") as f:
            self.metadata = json.load(f)

        self.feature_dim = int(self.metadata["feature_dimension"])
        self.class_names = list(self.metadata["class_names"])

        inp = self.session.get_inputs()[0]
        shape = inp.shape
        if isinstance(shape, (list, tuple)) and len(shape) >= 3:
            last = shape[-1]
            if isinstance(last, int) and last > 0 and last != self.feature_dim:
                raise ValueError(
                    f"ONNX 输入最后一维为 {last}，与 metadata 中 feature_dimension={self.feature_dim} 不一致"
                )

        n_scaler = int(self.scaler.n_features_in_) if hasattr(self.scaler, "n_features_in_") else None
        if n_scaler is not None and n_scaler != self.feature_dim:
            raise ValueError(
                f"scaler 期望特征数 {n_scaler}，与 metadata feature_dimension={self.feature_dim} 不一致"
            )

        print("模型加载完成", file=sys.stderr)
        print(f"- ONNX: {self.onnx_path}", file=sys.stderr)
        print(f"- 标准化器: {self.scaler_path}", file=sys.stderr)
        print(f"- 输入名: {self.input_name}, 输出名: {self.output_name}", file=sys.stderr)
        print(f"- 特征维数: {self.feature_dim}, 类别数: {len(self.class_names)}", file=sys.stderr)

    def _ensure_sample_rows(self, data: np.ndarray, *, verbose: bool = True) -> np.ndarray:
        if data.ndim != 2:
            raise ValueError(f"特征必须是二维 (样本数, 维数)，当前 shape={data.shape}")
        if self.feature_dim is None:
            raise RuntimeError("请先 load_model()")

        fd = self.feature_dim
        if data.shape[1] == fd:
            return data
        if data.shape[0] == fd:
            if verbose:
                print(f"检测到特征在行方向，转置为 (N, {fd}): {data.shape} -> {data.T.shape}")
            return data.T

        raise ValueError(
            f"无法识别数据布局，期望某一维等于特征维 {fd}，当前 shape={data.shape}。"
            f"请使用每行一个样本、每列一维特征，形状 (样本数, {fd})。"
        )

    def preprocess(self, features: np.ndarray, *, verbose: bool = True) -> np.ndarray:
        """返回 ONNX 输入 float32，形状 ``(N, 1, L)``。"""
        x = self._ensure_sample_rows(features.astype(np.float64), verbose=verbose)
        if verbose:
            print(f"预处理后特征形状 (N, L): {x.shape}")
        if self.feature_dim is not None and x.shape[1] != self.feature_dim:
            raise ValueError(f"特征列数为 {x.shape[1]}，需要 {self.feature_dim}")
        scaled = self.scaler.transform(x).astype(np.float32)
        return np.expand_dims(scaled, axis=1)

    def infer(
        self,
        features: np.ndarray,
        *,
        verbose: bool = True,
    ) -> Tuple[np.ndarray, np.ndarray]:
        """底层推理：``(N, 256)`` → ``(pred_labels, logits)``。实时循环请传 ``verbose=False``。"""
        if self.session is None:
            raise RuntimeError("请先 load_model()")
        x = self.preprocess(features, verbose=verbose)
        logits = self.session.run([self.output_name], {self.input_name: x})[0]
        preds = np.argmax(logits, axis=1).astype(np.int64)
        return preds, logits

    def diagnose_batch(
        self,
        features: np.ndarray,
        *,
        verbose: bool = False,
    ) -> Tuple[np.ndarray, np.ndarray]:
        """
        **推荐对外接口**：无标签批量诊断，``features`` 形状 ``(N, feature_dim)``。

        返回
        ----
        pred_labels : (N,) int64
        logits : (N, num_classes) float32
        """
        return self.infer(features, verbose=verbose)

    def diagnose_one(
        self,
        sample_256: np.ndarray,
        *,
        verbose: bool = False,
    ) -> Tuple[int, np.ndarray]:
        """
        **推荐对外接口**：单条实时样本，``sample_256`` 为长度 ``feature_dim`` 的一维向量，
        或形状 ``(1, feature_dim)``。
        """
        x = np.asarray(sample_256, dtype=np.float64)
        if x.ndim == 1:
            if x.shape[0] != self.feature_dim:
                raise ValueError(f"单条样本长度应为 {self.feature_dim}，当前 {x.shape[0]}")
            x = x.reshape(1, -1)
        elif x.ndim == 2 and x.shape in [(1, self.feature_dim), (self.feature_dim, 1)]:
            if x.shape[0] == self.feature_dim:
                x = x.T
        else:
            raise ValueError(f"单条样本形状应为 ({self.feature_dim},) 或 (1,{self.feature_dim})，当前 {x.shape}")

        preds, logits = self.infer(x, verbose=verbose)
        return int(preds[0]), logits[0].astype(np.float64)

    # ----- 与训练脚本一致的 txt 行解析（保留给文件批处理 / 你自己读串口后组行） -----

    def parse_training_txt_line(self, line: str) -> np.ndarray | None:
        """
        解析一行训练格式：制表符分隔，``parts[1:feature_dim+1]`` 为特征（与训练代码一致）。
        非法行返回 ``None``。
        """
        if self.feature_dim is None:
            raise RuntimeError("请先 load_model()")
        line = line.strip()
        if not line:
            return None
        parts = line.split("\t")
        fd = self.feature_dim
        if len(parts) < fd + 1:
            return None
        try:
            return np.array([float(x) for x in parts[1 : fd + 1]], dtype=np.float64)
        except ValueError:
            return None

    def load_training_format_txt(self, path: str | Path) -> np.ndarray:
        """读取整文件，行格式与 ``read_and_classify_data`` 中单行规则一致。"""
        if self.feature_dim is None:
            raise RuntimeError("请先 load_model()")
        path = Path(path)
        rows: list[np.ndarray] = []
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                v = self.parse_training_txt_line(line)
                if v is not None:
                    rows.append(v)
        if not rows:
            raise ValueError(
                f"未从 {path} 解析到有效行：需制表符分隔，首列+后续 {self.feature_dim} 列数值特征"
            )
        return np.stack(rows, axis=0)

    @staticmethod
    def parse_pure256_line(line: str, feature_dim: int) -> np.ndarray | None:
        """解析一行纯特征：256 个 float，分隔符为制表符或逗号（自动二选一）。"""
        line = line.strip()
        if not line:
            return None
        if "\t" in line:
            parts = line.split("\t")
        else:
            parts = line.split(",")
        if len(parts) < feature_dim:
            return None
        parts = parts[:feature_dim]
        try:
            return np.array([float(x) for x in parts], dtype=np.float64)
        except ValueError:
            return None

    def _peek_txt_layout(self, path: Path) -> InputFileFormat:
        """根据首行非空行判断 ``training_txt`` 还是 ``pure256``。"""
        if self.feature_dim is None:
            raise RuntimeError("请先 load_model()")
        fd = self.feature_dim
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.strip()
                if not s:
                    continue
                n_tab = s.count("\t")
                parts_tab = s.split("\t")
                if len(parts_tab) >= fd + 1:
                    return "training_txt"
                if len(parts_tab) == fd or ("," not in s and n_tab >= fd - 1):
                    return "pure256"
                parts_c = s.split(",")
                if len(parts_c) >= fd:
                    return "pure256"
                return "pure256"
        return "pure256"

    def load_pure256_table_file(self, path: str | Path) -> np.ndarray:
        """纯数值表：``(N, feature_dim)``，逗号或制表符（由首行 sniff）。"""
        path = Path(path)
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            first = ""
            for line in f:
                s = line.strip()
                if s:
                    first = s
                    break
        if not first:
            raise ValueError(f"文件为空: {path}")
        delim = "\t" if first.count("\t") >= self.feature_dim - 1 else ","
        arr = np.loadtxt(path, delimiter=delim)
        return np.atleast_2d(arr)

    def load_feature_file(
        self,
        path: str | Path,
        *,
        format: InputFileFormat = "auto",
    ) -> np.ndarray:
        """
        从文件加载特征矩阵 ``(N, feature_dim)``。

        - ``npy``：直接 ``np.load``。
        - ``auto``：``.npy`` 直读；``.txt`` 用首行判断 training / pure；``.csv`` 按纯表读入。
        """
        if self.feature_dim is None:
            raise RuntimeError("请先 load_model()")
        path = Path(path)
        suffix = path.suffix.lower()
        if suffix == ".npy":
            return np.load(path)

        fmt: InputFileFormat = format
        if fmt == "auto":
            if suffix == ".csv":
                fmt = "pure256"
            elif suffix == ".txt":
                fmt = self._peek_txt_layout(path)
            else:
                fmt = "pure256"

        if fmt == "training_txt":
            return self.load_training_format_txt(path)
        return self.load_pure256_table_file(path)

    def load_mat_labeled(self, mat_path: str | Path) -> Tuple[np.ndarray, np.ndarray]:
        from scipy.io import loadmat  # 惰性导入：PS 上不跑 mat 可不装 scipy

        if self.feature_dim is None:
            raise RuntimeError("请先 load_model()")
        mat_path = Path(mat_path)
        d = loadmat(mat_path)
        if "labeled_data" not in d:
            keys = [k for k in d.keys() if not k.startswith("__")]
            raise ValueError(f".mat 中需要变量 labeled_data，当前可用: {keys}")
        data = d["labeled_data"]
        need = 1 + self.feature_dim
        if data.shape[1] < need:
            raise ValueError(f"列数 {data.shape[1]} 需要至少 {need}（1 列标签 + {self.feature_dim} 维特征）")
        y = data[:, 0].astype(np.int64).ravel()
        feat = data[:, 1 : 1 + self.feature_dim]
        return feat, y

    def print_prediction_summary(
        self,
        preds: np.ndarray,
        logits: np.ndarray,
        *,
        max_preview: int = 15,
        title: str = "诊断结果（控制台摘要）",
    ) -> None:
        n = len(preds)
        print(f"\n========== {title} ==========")
        print(f"样本数: {n}")
        unique, counts = np.unique(preds, return_counts=True)
        print("预测类别分布:")
        for lab, cnt in zip(unique.astype(int), counts):
            name = self.class_names[int(lab)]
            print(f"  [{lab}] {name}: {int(cnt)} 条 ({cnt / n:.1%})")
        if max_preview <= 0:
            return
        show = min(max_preview, n)
        print(f"\n前 {show} 条预览（index / 预测类别 / 名称 / logits 最大分量）:")
        for i in range(show):
            pi = int(preds[i])
            mx = float(np.max(logits[i]))
            print(f"  {i}\t{pi}\t{self.class_names[pi]}\tmax_logit={mx:.4f}")

    def evaluate_with_labels(
        self,
        y_true: np.ndarray,
        preds: np.ndarray,
        logits: np.ndarray,
        output_dir: Path,
    ) -> None:
        from sklearn.metrics import (
            accuracy_score,
            classification_report,
            confusion_matrix,
            f1_score,
            precision_score,
            recall_score,
        )

        output_dir = Path(output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)

        acc = accuracy_score(y_true, preds)
        p = precision_score(y_true, preds, average="weighted", zero_division=0)
        r = recall_score(y_true, preds, average="weighted", zero_division=0)
        f1 = f1_score(y_true, preds, average="weighted", zero_division=0)
        cm = confusion_matrix(y_true, preds)

        print("\n========== 诊断结果（带标签） ==========")
        print(f"准确率: {acc:.4f}")
        print(f"加权精确率: {p:.4f}, 加权召回: {r:.4f}, 加权 F1: {f1:.4f}")
        print("混淆矩阵 (行=真实, 列=预测):")
        print(cm)
        print("\n分类报告:")
        print(
            classification_report(
                y_true,
                preds,
                target_names=self.class_names,
                zero_division=0,
            )
        )

        out = output_dir / "mscnn_predictions.txt"
        with open(out, "w", encoding="utf-8") as f:
            f.write("index\ttrue_label\tpred_label\ttrue_name\tpred_name\tmax_logit\n")
            for i in range(len(preds)):
                ti, pi = int(y_true[i]), int(preds[i])
                mx = float(np.max(logits[i]))
                f.write(
                    f"{i}\t{ti}\t{pi}\t{self.class_names[ti]}\t{self.class_names[pi]}\t{mx:.6f}\n"
                )
        print(f"已保存: {out}")

    def run_mat(self, mat_path: str | Path, output_dir: str | Path, *, max_preview: int = 15) -> None:
        feat, y_true = self.load_mat_labeled(mat_path)
        preds, logits = self.infer(feat, verbose=True)
        self.print_prediction_summary(
            preds, logits, max_preview=max_preview, title="预测摘要（完整评估见下方）"
        )
        self.evaluate_with_labels(y_true, preds, logits, Path(output_dir))

    def run_feature_file(
        self,
        feature_path: str | Path,
        output_dir: str | Path,
        *,
        input_format: InputFileFormat = "auto",
        max_preview: int = 15,
    ) -> None:
        feat = self.load_feature_file(feature_path, format=input_format)
        preds, logits = self.infer(feat, verbose=True)
        self.print_prediction_summary(
            preds,
            logits,
            max_preview=max_preview,
            title="诊断结果（无真实标签，仅预测）",
        )
        out_dir = Path(output_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        out = out_dir / "mscnn_predictions_no_label.txt"
        with open(out, "w", encoding="utf-8") as f:
            f.write("index\tpred_label\tpred_name\tmax_logit\n")
            for i in range(len(preds)):
                pi = int(preds[i])
                mx = float(np.max(logits[i]))
                f.write(f"{i}\t{pi}\t{self.class_names[pi]}\t{mx:.6f}\n")
        print(f"\n完整逐条结果已写入: {out.resolve()}")
        print("（无真实标签；可选 .mat 评估见 --input_type mat）")

    def run_stdin_loop(
        self,
        *,
        stdin_layout: Literal["pure256", "training_txt"] = "pure256",
        flush: bool = True,
    ) -> None:
        """
        从标准输入逐行读入、逐行输出诊断（适合管道 / 采集进程 ``stdout`` 对接）。

        每行输出：``pred_label \\t class_name \\t max_logit``（无标签）。
        """
        if self.feature_dim is None:
            raise RuntimeError("请先 load_model()")
        fd = self.feature_dim
        print(
            f"stdin 模式已启动（layout={stdin_layout}），每行 {fd} 维特征；"
            "输出: pred_label\\tclass_name\\tmax_logit",
            file=sys.stderr,
        )
        for line in sys.stdin:
            if stdin_layout == "training_txt":
                vec = self.parse_training_txt_line(line)
            else:
                vec = self.parse_pure256_line(line, fd)
            if vec is None:
                continue
            pred, logit = self.diagnose_one(vec, verbose=False)
            mx = float(np.max(logit))
            out = f"{pred}\t{self.class_names[pred]}\t{mx:.6f}\n"
            sys.stdout.write(out)
            if flush:
                sys.stdout.flush()


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="ONNX Runtime 多尺度 CNN 腐蚀诊断（文件批处理 / stdin 实时流）"
    )
    p.add_argument("--model_dir", type=str, default=str(DEFAULT_MODEL_DIR), help="onnx / scaler / metadata 所在目录")
    p.add_argument(
        "--mode",
        type=str,
        choices=["file", "stdin"],
        default="file",
        help="file=读 --input_path；stdin=从标准输入逐行读特征并输出预测",
    )
    p.add_argument("--input_type", type=str, choices=["mat", "feature"], default="feature")
    p.add_argument(
        "--input_path",
        type=str,
        default="",
        help="mode=file 时使用。feature 留空则默认尝试 data/1.txt（训练格式 txt）",
    )
    p.add_argument(
        "--input_format",
        type=str,
        choices=["auto", "pure256", "training_txt"],
        default="auto",
        help="feature 文件布局：auto 自动判断；pure256=每行仅256数；training_txt=首列+256列（与训练txt一致）",
    )
    p.add_argument(
        "--stdin_layout",
        type=str,
        choices=["pure256", "training_txt"],
        default="pure256",
        help="mode=stdin 时每行格式（实时采集一般为 pure256）",
    )
    p.add_argument("--output_dir", type=str, default=str(DEFAULT_OUTPUT_DIR))
    p.add_argument(
        "--preview",
        type=int,
        default=15,
        help="批处理模式下终端预览条数；0=仅分布",
    )
    return p


def main() -> None:
    args = build_arg_parser().parse_args()
    clf = MSCNNOnnxRuntimeClassifier(model_dir=Path(args.model_dir))
    clf.load_model()

    if args.mode == "stdin":
        clf.run_stdin_loop(stdin_layout=args.stdin_layout)
        return

    input_path = args.input_path.strip()
    if args.input_type == "mat":
        if not input_path:
            raise ValueError("mat 模式请用 --input_path 指定 .mat（内含变量 labeled_data）")
        if not os.path.isfile(input_path):
            raise FileNotFoundError(input_path)
        clf.run_mat(input_path, args.output_dir, max_preview=args.preview)
    else:
        if not input_path:
            input_path = str(DEFAULT_FEATURE_FILE)
            print(f"未指定 --input_path，使用默认: {input_path}", file=sys.stderr)
        if not os.path.isfile(input_path):
            raise FileNotFoundError(
                f"找不到输入文件: {input_path}\n"
                "请指定 --input_path，或放置训练用 data/1.txt；实时流请使用 --mode stdin"
            )
        clf.run_feature_file(
            input_path,
            args.output_dir,
            input_format=args.input_format,
            max_preview=args.preview,
        )


if __name__ == "__main__":
    main()
