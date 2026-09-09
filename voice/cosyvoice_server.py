#!/home/yc_21/miniconda3/envs/cosyvoice/bin/python
# -*- coding: utf-8 -*-
"""
CosyVoice2-0.5B TTS sidecar: 模型常驻 GPU, 为 llm-server 提供逐句合成。
- POST /tts  {text, voice?} -> 返回 wav 字节 (audio/wav, 22050Hz 16bit mono)
- GET  /health -> {"ok": true}
- 语速由浏览器端 playbackRate 控制(CosyVoice 原速合成, 浏览器 time-stretch 无音调失真)
用法: python tts_cosyvoice_server.py --port 9101 --model <CosyVoice2-0.5B目录>
"""
import io, json, wave, argparse, logging, threading
import numpy as np
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("cosy")

_model, _spks, _lock = None, [], threading.Lock()
DEFAULT_VOICE = "中文女"

def load_cosy(model_dir: str):
    global _model, _spks
    from cosyvoice.cli.cosyvoice import CosyVoice2
    _model = CosyVoice2(model_dir, load_jit=False, load_trt=False, fp16=False)
    _spks = _model.list_available_spks()
    log.info("CosyVoice2 loaded, speakers=%s", _spks)

def tensor_to_wav(speech) -> bytes:
    a = speech.detach().cpu().numpy().squeeze()
    a = np.clip(a, -1.0, 1.0)
    pcm = (a * 32767).astype(np.int16)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050)
        w.writeframes(pcm.tobytes())
    return buf.getvalue()

def synth(text, voice):
    global _model, _spks
    if not _model: return None
    if voice not in _spks: voice = DEFAULT_VOICE if DEFAULT_VOICE in _spks else (_spks[0] if _spks else None)
    if not voice: return None
    with _lock:  # 同一实例串行推理
        for out in _model.inference_sft(text, voice, stream=False):
            wav = tensor_to_wav(out["tts_speech"])
            return wav
    return None

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _read_body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        return self.rfile.read(n) if n else b""
    def do_GET(self):
        if self.path.startswith("/health"):
            self.send_response(200); self.send_header("Content-Type", "application/json")
            self.end_headers(); self.wfile.write(b'{"ok": true}')
        else:
            self.send_response(404); self.end_headers()
    def do_POST(self):
        if not self.path.startswith("/tts"):
            self.send_response(404); self.end_headers(); return
        try:
            data = json.loads(self._read_body().decode("utf-8", "ignore"))
            text = (data.get("text") or "").strip()
            voice = data.get("voice") or DEFAULT_VOICE
        except Exception:
            self.send_response(400); self.end_headers(); return
        wav = synth(text, voice) if text else None
        if not wav:
            self.send_response(500); self.end_headers(); return
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(wav)))
        self.end_headers()
        self.wfile.write(wav)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9101)
    ap.add_argument("--model", required=True)
    args = ap.parse_args()
    load_cosy(args.model)
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), H)
    log.info("listening on :%d speakers=%s", args.port, _spks)
    srv.serve_forever()

if __name__ == "__main__":
    main()