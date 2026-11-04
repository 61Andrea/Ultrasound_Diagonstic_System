import joblib
import json

scaler = joblib.load("scaler.pkl")

data = {
    "mean": scaler.mean_.tolist(),
    "scale": scaler.scale_.tolist()
}

with open("scaler.json", "w", encoding="utf-8") as f:
    json.dump(data, f)

print("已生成 scaler.json")
print("特征维度:", len(data["mean"]))