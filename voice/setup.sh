#!/usr/bin/env bash
# V1 语音引擎安装/构建: 本地 ASR(whisper.cpp) + 本地 TTS(piper 预编译)
# 产物均在 ./ (模型/二进制不提交 git); 重跑可幂等
set -e
cd "$(dirname "$0")"
mkdir -p whisper-models piper-voices

DL() { # DL <url> <out> —— 优先 hf-mirror(国内), 回退 huggingface/github
  curl -fsSL --max-time 600 -o "$2" "$1" || echo "[skip] $1"
}

echo "[1/5] whisper.cpp 源码克隆..."
if [ ! -f whisper.cpp/CMakeLists.txt ]; then
  git clone --depth 1 https://github.com/ggml-org/whisper.cpp.git || { echo "github clone 失败"; exit 1; }
fi

echo "[2/5] whisper base 模型 (~142MB)..."
if [ ! -f whisper-models/ggml-base.bin ]; then
  DL https://hf-mirror.com/ggerganov/whisper.cpp/resolve/main/ggml-base.bin whisper-models/ggml-base.bin
  [ -s whisper-models/ggml-base.bin ] || DL https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin whisper-models/ggml-base.bin
  [ -s whisper-models/ggml-base.bin ] || { echo "model download 失败"; exit 1; }
fi

echo "[3/5] piper 预编译 pyi (GitHub)..."
if [ ! -x piper/piper ]; then
  mkdir -p piper
  curl -fsSL --max-time 600 -o piper/piper.tar.gz \
    https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz
  ( cd piper && tar xzf piper.tar.gz && rm piper.tar.gz )
  chmod +x piper/piper 2>/dev/null || true
fi

echo "[4/5] piper 中文音色 zh_CN-huayan-medium (~63MB)..."
if [ ! -f piper-voices/zh_CN-huayan-medium.onnx ]; then
  DL https://hf-mirror.com/rhasspy/piper-voices/resolve/main/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx piper-voices/zh_CN-huayan-medium.onnx
  [ -s piper-voices/zh_CN-huayan-medium.onnx ] || DL https://huggingface.co/rhasspy/piper-voices/resolve/main/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx piper-voices/zh_CN-huayan-medium.onnx
  [ -s piper-voices/zh_CN-huayan-medium.onnx ] || { echo "voice onnx 失败"; exit 1; }
fi
if [ ! -f piper-voices/zh_CN-huayan-medium.onnx.json ]; then
  DL https://hf-mirror.com/rhasspy/piper-voices/resolve/main/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx.json piper-voices/zh_CN-huayan-medium.onnx.json
  [ -s piper-voices/zh_CN-huayan-medium.onnx.json ] || DL https://huggingface.co/rhasspy/piper-voices/resolve/main/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx.json piper-voices/zh_CN-huayan-medium.onnx.json
fi

echo "[5/5] 构建 whisper.cpp (CPU)..."
( cd whisper.cpp && cmake -B build -DCMAKE_BUILD_TYPE=Release -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=OFF >/dev/null 2>&1 || cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
  PATH=/usr/bin:/bin cmake --build build -j"$(nproc)" >/dev/null 2>&1 || PATH=/usr/bin:/bin cmake --build build -j"$(nproc)" --target whisper-cli main >/dev/null 2>&1 || true )

echo "=== whisper 可执行 ==="
ls -l whisper.cpp/build/bin/ 2>/dev/null || ls -l whisper.cpp/build/main whisper.cpp/build/whisper-cli 2>/dev/null || echo "whisper-cli 未找到(检查构建日志)"
echo "=== piper ==="
./piper/piper --help 2>&1 | head -3 || true
echo "SETUP_DONE"