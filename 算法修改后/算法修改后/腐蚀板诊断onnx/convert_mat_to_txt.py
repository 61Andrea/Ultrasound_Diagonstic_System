from scipy.io import loadmat
import numpy as np


# 修改成你的 mat 文件名
MAT_PATH = "ceshijiao_400labeled.mat"

# 输出给板子用的 txt
OUT_PATH = "jt.txt"


mat = loadmat(MAT_PATH)

print("mat 文件中的变量：")
for key in mat.keys():
    if not key.startswith("__"):
        print(key, mat[key].shape)


# 如果你的 mat 里变量名是 labeled_data，用这一句
data = mat["labeled_data"]

# 如果前两列是标签，第 3 列以后是 256 维特征
x = data[:, 2:]

print("原始数据维度:", data.shape)
print("导出特征维度:", x.shape)

if x.shape[1] != 256:
    raise ValueError("特征维度不是 256，当前是 {}".format(x.shape[1]))

# 保存成 txt，每一行一个样本，256 个特征，用空格分隔
np.savetxt(OUT_PATH, x, fmt="%.8f")

print("已生成:", OUT_PATH)
