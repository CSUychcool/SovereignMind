#!/home/yc_21/miniconda3/envs/cosyvoice/bin/python
# -*- coding: utf-8 -*-
"""
CosyVoice2-0.5B TTS sidecar: 模型常驻 GPU, 为 llm-server 提供逐句合成。
- POST /tts  {text, voice?} -> 返回 wav 字节 (audio/wav, 22050Hz 16bit mono)
- GET  /health -> {"ok": true}
- 语速由浏览器端 playbackRate 控制(CosyVoice 原速合成, 浏览器 time-stretch 无音调失真)
用法: python tts_cosyvoice_server.py --port 9101 --model <CosyVoice2-0.5B目录>
"""
import io, os, json, wave, argparse, logging, threading, hashlib
import numpy as np
import torch
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("cosy")

_model, _spks, _lock = None, [], threading.Lock()
BASE = os.path.dirname(os.path.abspath(__file__))
VOICES_DIR = os.path.join(BASE, "cosyvoice-voices")
DEFAULT_PROMPT_WAV = os.path.join(BASE, "cosyvoice-prompt", "zero_shot_prompt.wav")
_voices = {}   # name -> (wav, txt)
_prompt_text = None

def load_voices():
    """扫描 cosyvoice-voices/*.wav (+同名 .txt) 作为可用音色表"""
    global _voices
    _voices = {}
    if not os.path.isdir(VOICES_DIR):
        return
    for name in sorted(os.listdir(VOICES_DIR)):
        wav = os.path.join(VOICES_DIR, name)
        if not (name.endswith(".wav") and os.path.isfile(wav)):
            continue
        txt = os.path.join(VOICES_DIR, name[:-4] + ".txt")
        _voices[name[:-4]] = (wav, txt if os.path.isfile(txt) else "")

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

def default_prompt_text():
    global _prompt_text
    if _prompt_text is None:
        try:
            with open(DEFAULT_PROMPT_WAV + ".txt", "r", encoding="utf-8") as f:
                _prompt_text = f.read().strip()
        except Exception:
            _prompt_text = ""
    return _prompt_text

# ---- 音色特征缓存: 每个音色的 prompt 特征只提取一次, 之后每句跳过 CPU 特征提取 ----
_voice_cache = {}   # key -> dict(prompt 侧特征)
USE_CACHE = os.environ.get("VOICE_CACHE") == "1"   # 默认关闭(原生推理最稳), 需要加速再开

def build_prompt_cache(pwav, ptext):
    front = _model.frontend
    prompt_norm = front.text_normalize(ptext, split=False)
    p_token, p_len = front._extract_text_token(prompt_norm)
    feat, feat_len = front._extract_speech_feat(pwav)          # prompt 波形特征(24k)
    token, token_len = front._extract_speech_token(pwav)       # prompt 语音 token
    emb = front._extract_spk_embedding(pwav)                   # 说话人特征
    return {'prompt_text': p_token, 'prompt_text_len': p_len,
            'feat': feat, 'feat_len': feat_len,
            'speech_token': token, 'speech_token_len': token_len,
            'embedding': emb}

def synth_with_cache(text, key):
    """用缓存好的 prompt 特征合成; 与 inference_zero_shot 同键名镜像"""
    front = _model.frontend
    base = _voice_cache[key]
    outs = []
    for t in front.text_normalize(text, split=True):
        if not isinstance(t, str):
            continue
        t_tok, t_len = front._extract_text_token(t)            # 每句只需 tokenize 文本(便宜)
        token_len = min(int(base['feat'].shape[1] / 2), base['speech_token'].shape[1])
        feat_s = base['feat'][:, :2 * token_len]
        token_s = base['speech_token'][:, :token_len]
        model_input = {'text': t_tok, 'text_len': t_len,
                       'prompt_text': base['prompt_text'], 'prompt_text_len': base['prompt_text_len'],
                       'llm_prompt_speech_token': token_s, 'llm_prompt_speech_token_len': token_len,
                       'flow_prompt_speech_token': token_s, 'flow_prompt_speech_token_len': token_len,
                       'prompt_speech_feat': feat_s, 'prompt_speech_feat_len': 2 * token_len,
                       'llm_embedding': base['embedding'], 'flow_embedding': base['embedding']}
        for mo in _model.model.tts(**model_input, stream=False, speed=1.0):
            outs.append(mo['tts_speech'])
    if not outs:
        return None
    speech = torch.cat(outs, dim=1) if len(outs) > 1 else outs[0]
    return tensor_to_wav(speech)

def synth(text, voice, prompt_text):
    """音色策略:
       1) 有内置音色(cosyvoice2 内置 spk) -> SFT
       2) 否则零样本克隆: voice=音色名(cosyvoice-voices) 或 wav 路径; 缺省取库内第一个
       prompt 特征按音色缓存一次, 每句只 tokenize 文本 + GPU 生成
    """
    global _model, _spks, _voice_cache
    if not _model:
        return None
    with _lock:
        if _spks:
            spk = voice if (voice and voice in _spks) else (_spks[0] if _spks else None)
            if spk:
                for out in _model.inference_sft(text, spk, stream=False):
                    return tensor_to_wav(out["tts_speech"])
        pwav, ptext = DEFAULT_PROMPT_WAV, None
        if voice and voice in _voices:
            pwav, ptext = _voices[voice]
        elif voice and os.path.isfile(voice):
            pwav = voice
        elif _voices:
            pwav, ptext = next(iter(_voices.values()))
        if not os.path.isfile(pwav):
            log.warning("缺零样本 prompt 音频: %s", pwav)
            return None
        ptext = (prompt_text or ptext or default_prompt_text()) or text
        # 缓存路径仅在 VOICE_CACHE=1 时启用(自测: 加速但更易出误差)
        if USE_CACHE:
            key = voice if voice in _voices else ("path:" + pwav)
            if prompt_text:
                key = key + ":" + hashlib.md5(ptext.encode()).hexdigest()[:8]
            try:
                if key not in _voice_cache:
                    _voice_cache[key] = build_prompt_cache(pwav, ptext)
                wav = synth_with_cache(text, key)
                if wav:
                    return wav
                log.warning("cached synth 无输出, 走原生")
            except Exception as e:
                log.warning("cached synth 失败(%s), 走原生", e)
        # 原生零样本(整段一口, sidecar 内部切句); 官方实现最稳
        parts = []
        for out in _model.inference_zero_shot(text, ptext, pwav, stream=False):
            parts.append(out["tts_speech"])
        if parts:
            speech = torch.cat(parts, dim=1) if len(parts) > 1 else parts[0]
            return tensor_to_wav(speech)
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
        elif self.path.startswith("/voices"):
            self.send_response(200); self.send_header("Content-Type", "application/json")
            self.end_headers()
            import json as _j
            self.wfile.write(_j.dumps(sorted(_voices.keys())).encode())
        else:
            self.send_response(404); self.end_headers()
    def do_POST(self):
        if not self.path.startswith("/tts"):
            self.send_response(404); self.end_headers(); return
        try:
            data = json.loads(self._read_body().decode("utf-8", "ignore"))
            text = (data.get("text") or "").strip()
            voice = data.get("voice") or ""                    # 音色: 内置名 或 自定义 wav 路径(定制/克隆)
            prompt_text = data.get("prompt_text") or ""        # 零样本时可选
        except Exception:
            self.send_response(400); self.end_headers(); return
        wav = synth(text, voice, prompt_text) if text else None
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
    load_voices()
    log.info("voices=%s", list(_voices.keys()))
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), H)
    log.info("listening on :%d speakers=%s", args.port, _spks)
    srv.serve_forever()

if __name__ == "__main__":
    main()