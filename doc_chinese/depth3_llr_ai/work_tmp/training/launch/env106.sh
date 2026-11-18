# 106 的模式切换：有 eGPU 就用 GPU，没接盒子就自动用 CPU。
#
# 为什么需要这个文件而不是直接 export CUDA 变量：106 的盒子不常插，同一个 venv 要在两种硬件下都能跑。
# TensorFlow 本身会自动回退（没有 GPU 时用 CPU），但 ① 找不到 CUDA 库时会静默跑 CPU，② 找不到设备时
# 仍会打印一堆 CUDA 错误。所以这里只做两件事：把 nvidia wheels 的 lib 加进 LD_LIBRARY_PATH（存在才加），
# 以及提供一个显式的 CPU 开关（OCUDU_FORCE_CPU=1 时把 CUDA_VISIBLE_DEVICES 清空）。
NV=$(ls -d "$HOME"/work/ai_phy_train/.venv/lib/python3.12/site-packages/nvidia/*/lib 2>/dev/null | tr "\n" ":")
if [ -n "$NV" ]; then
  export LD_LIBRARY_PATH="$NV${LD_LIBRARY_PATH:-}"
fi
if [ "${OCUDU_FORCE_CPU:-0}" = "1" ]; then
  export CUDA_VISIBLE_DEVICES=""
fi
# 一眼看出这次会跑在哪：GPU 名字，或者 "CPU"
if [ "${OCUDU_FORCE_CPU:-0}" = "1" ] || ! command -v nvidia-smi >/dev/null 2>&1 || ! nvidia-smi -L >/dev/null 2>&1; then
  echo "[env106] mode: CPU"
else
  echo "[env106] mode: $(nvidia-smi --query-gpu=name --format=csv,noheader | head -1)"
fi
