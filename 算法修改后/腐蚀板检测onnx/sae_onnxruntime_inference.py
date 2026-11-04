"""
sae_onnxruntime_inference.py — SAE 异常检测 ONNX Runtime 推理
=============================================================
支持两种输入模式:
  --mode feature : 直接特征 CSV (如 1-4.csv), 每行 256 维特征
  --mode voltage : 电压域测试文件, 先 μ-law 逆映射再推理

部署文件依赖:
  model/sae_detector_model.onnx   — 推理图
  model/sae_detector_scaler.pkl   — StandardScaler
  model/sae_detector_metadata.json — 阈值 / 参数

用法:
  python sae_onnxruntime_inference.py --mode feature --input 2.csv
  python sae_onnxruntime_inference.py --mode voltage --input data_test/异常数据/异常1.csv
  python sae_onnxruntime_inference.py --mode voltage_dir --input data_test/正常数据
"""
import argparse, json, os, sys, glob, time
from pathlib import Path
from typing import Tuple
import joblib, numpy as np, pandas as pd
import onnxruntime as ort

MODEL_DIR = Path("model")
ONNX_NAME = "sae_detector_model.onnx"
SCALER_NAME = "sae_detector_scaler.pkl"
METADATA_NAME = "sae_detector_metadata.json"

# μ-law 逆映射参数（与标定过程相关，随模型权重文件一并交付）
MU, LN_1P_MU = 255, 5.545177444479562  # PLACEHOLDER: 以正式交付参数为准
V_MIN, V_MAX = 0.0, 2403.0             # PLACEHOLDER: 以正式交付参数为准
MAPPING_V_MAX = 8.0                    # PLACEHOLDER: 以正式交付参数为准


def voltage_to_features(v: np.ndarray) -> np.ndarray:
    """电压域 → 特征域 (μ-law 逆映射)"""
    v_norm = np.clip(v / MAPPING_V_MAX, 0.0, 1.0)
    x_norm = (np.exp(v_norm * LN_1P_MU) - 1.0) / MU
    return (x_norm * (V_MAX - V_MIN) + V_MIN).astype(np.float32)


class SAEOnnxDetector:
    """SAE 异常检测器 (ONNX Runtime)"""

    def __init__(self, model_dir: Path = MODEL_DIR):
        self.model_dir = Path(model_dir)
        self.session: ort.InferenceSession | None = None
        self.scaler = None
        self.metadata: dict = {}
        self.threshold: float = 0.0
        self.input_dim: int = 256

    def load(self):
        onnx_path = self.model_dir / ONNX_NAME
        scaler_path = self.model_dir / SCALER_NAME
        meta_path = self.model_dir / METADATA_NAME

        for p, name in [(onnx_path, "ONNX"), (scaler_path, "Scaler"), (meta_path, "Metadata")]:
            if not p.exists():
                raise FileNotFoundError(f"缺少部署文件: {p}")

        self.session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        self.scaler = joblib.load(scaler_path)
        with open(meta_path, "r", encoding="utf-8") as f:
            self.metadata = json.load(f)

        self.threshold = float(self.metadata["threshold"])
        self.input_dim = int(self.metadata["input_dim"])

        inp = self.session.get_inputs()[0]
        print(f"SAE 检测器加载完成")
        print(f"  ONNX: {onnx_path}")
        print(f"  输入: {inp.name} {inp.shape}")
        print(f"  阈值: {self.threshold:.4f}")
        print(f"  阈值方式: {self.metadata.get('threshold_meta', {}).get('k', 'N/A')}")

    def detect(self, features: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
        """
        批量检测. features: (N, input_dim) 已归一化的特征.
        返回: (pred_labels, mse_scores)
          pred_labels: 1=异常, 0=正常
          mse_scores: 每行重建误差
        """
        if features.ndim == 1:
            features = features.reshape(1, -1)
        if features.shape[1] != self.input_dim:
            raise ValueError(f"特征维度应为 {self.input_dim}, 当前 {features.shape[1]}")

        x = features.astype(np.float32)
        recon = self.session.run(None, {"input": x})[0]
        mse = np.mean((x - recon) ** 2, axis=1)
        preds = (mse > self.threshold).astype(np.int64)
        return preds, mse

    def detect_features(self, raw_features: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
        """原始特征 → [scaler] → 检测"""
        x = self.scaler.transform(raw_features).astype(np.float32)
        return self.detect(x)

    def detect_voltage(self, voltage: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
        """电压值 → [μ-law 逆映射] → [scaler] → 检测"""
        feats = voltage_to_features(voltage)
        return self.detect_features(feats)

    def predict_csv(self, csv_path: str):
        """对特征 CSV 文件推理"""
        print(f"\n读取特征文件: {csv_path}")
        df = pd.read_csv(csv_path, header=0 if "csv" in os.path.basename(csv_path) else None, dtype=np.float32)
        feats = df.values.astype(np.float32)
        print(f"  样本数: {feats.shape[0]}, 维度: {feats.shape[1]}")

        t0 = time.time()
        preds, mse = self.detect_features(feats)
        dt = time.time() - t0

        n_anomaly = int(preds.sum())
        n_normal = len(preds) - n_anomaly
        print(f"\n推理结果 ({dt*1000:.1f}ms):")
        print(f"  正常: {n_normal} ({n_normal/len(preds):.1%})")
        print(f"  异常: {n_anomaly} ({n_anomaly/len(preds):.1%})")
        print(f"  MSE 范围: [{mse.min():.4f}, {mse.max():.4f}], 均值: {mse.mean():.4f}")
        return preds, mse

    def predict_voltage_file(self, csv_path: str):
        """对单个电压域测试文件推理"""
        print(f"\n读取电压文件: {csv_path}")
        df = pd.read_csv(csv_path, header=None, dtype=np.float32)
        voltage = df.values.astype(np.float32)
        n_rows = voltage.shape[0]
        print(f"  形状: {n_rows} 行 × {df.shape[1]} 列 (电压域)")

        t0 = time.time()
        preds, mse = self.detect_voltage(voltage)
        dt = time.time() - t0

        n_anomaly = int(preds.sum())
        anomaly_ratio = n_anomaly / n_rows
        verdict = "异常(腐蚀)" if anomaly_ratio > 0.05 else "正常(健康)"

        print(f"推理 ({dt*1000:.1f}ms):")
        print(f"  行异常率: {anomaly_ratio:.1%} ({n_anomaly}/{n_rows})")
        print(f"  MSE 均值: {mse.mean():.4f}")
        print(f"  文件判定: {verdict}")
        return preds, mse, verdict

    def predict_voltage_dir(self, dir_path: str):
        """批量推理电压目录"""
        files = sorted(Path(dir_path).glob("*.csv"))
        print(f"\n批量推理: {len(files)} 个文件")

        results = []
        t0 = time.time()
        for f in files:
            df = pd.read_csv(f, header=None, dtype=np.float32)
            voltage = df.values.astype(np.float32)
            preds, mse = self.detect_voltage(voltage)
            ar = float(preds.mean())
            verdict = "异常" if ar > 0.05 else "正常"
            results.append({"file": f.name, "verdict": verdict, "anomaly_ratio": ar, "mean_mse": float(mse.mean())})

        dt = time.time() - t0
        n_anomaly = sum(1 for r in results if r["verdict"] == "异常")
        print(f"\n{'='*60}")
        print(f"汇总 ({dt:.1f}s): {len(files) - n_anomaly} 正常, {n_anomaly} 异常 / {len(files)} 个文件")
        print(f"{'='*60}")
        for r in results:
            print(f"  {r['verdict']:4s} | {r['file']:20s} | 行异常率={r['anomaly_ratio']:.1%} | MSE均值={r['mean_mse']:.4f}")
        return results


def main():
    parser = argparse.ArgumentParser(description="SAE 异常检测 ONNX 推理")
    parser.add_argument("--model_dir", type=str, default=str(MODEL_DIR))
    parser.add_argument("--mode", type=str, default="feature",
                        choices=["feature", "voltage", "voltage_dir"],
                        help="feature=特征CSV, voltage=电压测试文件, voltage_dir=批量电压目录")
    parser.add_argument("--input", type=str, required=True, help="输入文件/目录路径")
    args = parser.parse_args()

    detector = SAEOnnxDetector(Path(args.model_dir))
    detector.load()

    if args.mode == "feature":
        detector.predict_csv(args.input)
    elif args.mode == "voltage":
        detector.predict_voltage_file(args.input)
    elif args.mode == "voltage_dir":
        detector.predict_voltage_dir(args.input)


if __name__ == "__main__":
    main()
