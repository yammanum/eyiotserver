import os
import json
import base64
from datetime import datetime, timezone

from google import genai
from google.genai import types
from flask import Flask, jsonify, render_template, request

# Flask 앱 생성
app = Flask(__name__)

# Gemini API 설정 (Vercel 환경변수 사용)
API_KEY = os.getenv("GEMINI_API_KEY", "")
GEMINI_MODEL = "gemini-2.5-flash"

# 최신 센서 데이터를 메모리에 보관
_latest: dict = {}
_latest_image: dict = {}


def _decide_motors(temp: float, hum: float) -> tuple[bool, bool]:
    # 간단한 안전 규칙: 고온이면 팬 ON, 저습이면 펌프 ON
    fan_on = temp >= 28.0
    pump_on = hum <= 45.0
    return fan_on, pump_on


def _build_fallback_result(temp: float, hum: float, reason: str) -> str:
    fan_on, pump_on = _decide_motors(temp, hum)
    advice = "환기유지, 과습주의"
    if len(advice) > 20:
        advice = advice[:20]
    return json.dumps(
        {
            "fan": "ON" if fan_on else "OFF",
            "pump": "ON" if pump_on else "OFF",
            "advice": advice,
            "reason": reason[:80],
        },
        ensure_ascii=False,
    )


def _call_gemini(prompt: str) -> str:
    client = genai.Client(api_key=API_KEY)
    response = client.models.generate_content(
        model=GEMINI_MODEL,
        contents=prompt,
    )
    if not response.text:
        raise ValueError("Gemini response text is empty")
    return response.text


def _call_gemini_with_image(prompt: str, image_bytes: bytes, mime_type: str) -> str:
    client = genai.Client(api_key=API_KEY)
    image_part = types.Part.from_bytes(data=image_bytes, mime_type=mime_type)
    response = client.models.generate_content(
        model=GEMINI_MODEL,
        contents=[prompt, image_part],
    )
    if not response.text:
        raise ValueError("Gemini image response text is empty")
    return response.text


def _build_image_prompt(question: str, temp, hum) -> str:
    q = (question or "현재 식물 상태를 진단하고 관리 방법을 알려줘").strip()
    sensor_hint = ""
    if temp is not None and hum is not None:
        sensor_hint = f" 참고 센서값: temperature={temp}, humidity={hum}."
    return (
        "당신은 식물 생육 상태를 판단하는 전문가입니다. "
        "업로드된 식물 사진을 보고 질문에 답하세요. "
        f"질문: {q}."
        f"{sensor_hint}"
    )


@app.route("/", methods=["GET"])
def index():
    return render_template("index.html")


@app.route("/api/latest", methods=["GET"])
def api_latest():
    return jsonify(_latest)


@app.route("/api/latest-image", methods=["GET"])
def api_latest_image():
    return jsonify(_latest_image)


@app.route("/favicon.ico", methods=["GET"])
def favicon_ico():
    return "", 204


@app.route("/favicon.png", methods=["GET"])
def favicon_png():
    return "", 204


# (구) 헬스체크 엔드포인트 — 하위 호환용
@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok"})


@app.route("/sensor", methods=["POST"])
def sensor():
    try:
        data = request.get_json(force=True)
        if not data:
            return jsonify({"error": "JSON body is required"}), 400

        temp = data.get("temperature")
        hum = data.get("humidity")
        if temp is None or hum is None:
            return jsonify({"error": "temperature and humidity are required"}), 400

        print(f"[센서] 온도: {temp}, 습도: {hum}")

        prompt = (
            "아래 규칙을 반드시 지켜 한 줄 JSON만 출력하세요. "
            "추가 설명/코드블록/개행 금지. "
            f"입력값: temperature={temp}, humidity={hum}. "
            "출력 스키마: {\"fan\":\"ON|OFF\",\"pump\":\"ON|OFF\",\"advice\":\"20자 이내\"}. "
            "advice는 한국어 20자 이내로 작성하세요."
        )

        result = ""
        ai_error = None

        if API_KEY:
            try:
                result = _call_gemini(prompt)
            except Exception as e:
                ai_error = f"Gemini request failed: {e}"
        else:
            ai_error = "GEMINI_API_KEY is not set"

        if not result:
            result = _build_fallback_result(float(temp), float(hum), ai_error or "unknown")

        print(f"[AI 응답] {result}")

        # 최신 데이터 저장 (대시보드 표시용)
        _latest.update({
            "temperature": temp,
            "humidity": hum,
            "result": result,
            "timestamp": datetime.now(timezone.utc).isoformat(),
        })

        response = {"result": result}
        if ai_error:
            response["fallback"] = True
            response["detail"] = ai_error
        return jsonify(response)

    except Exception as e:
        print("에러:", e)
        return jsonify({"error": str(e)}), 500


@app.route("/plant-image", methods=["POST"])
def plant_image():
    try:
        if "image" not in request.files:
            return jsonify({"error": "multipart form-data field 'image' is required"}), 400

        file = request.files["image"]
        if not file or not file.filename:
            return jsonify({"error": "image file is empty"}), 400

        image_bytes = file.read()
        if not image_bytes:
            return jsonify({"error": "image file has no bytes"}), 400
        if len(image_bytes) > 4 * 1024 * 1024:
            return jsonify({"error": "image is too large (max 4MB)"}), 400

        mime_type = file.mimetype or "image/jpeg"
        if not mime_type.startswith("image/"):
            return jsonify({"error": "uploaded file must be an image"}), 400

        now_iso = datetime.now(timezone.utc).isoformat()
        _latest.update({"plant_timestamp": now_iso})

        _latest_image.update(
            {
                "timestamp": now_iso,
                "mime_type": mime_type,
                "image_b64": base64.b64encode(image_bytes).decode("ascii"),
            }
        )

        return jsonify({"ok": True, "timestamp": now_iso, "message": "image_saved"})

    except Exception as e:
        print("[plant-image] 에러:", e)
        return jsonify({"error": str(e)}), 500


@app.route("/api/analyze-image", methods=["POST"])
def analyze_image():
    try:
        if "image_b64" not in _latest_image or "mime_type" not in _latest_image:
            return jsonify({"error": "latest image not found"}), 400

        body = request.get_json(silent=True) or {}
        question = body.get("question", "")

        if not API_KEY:
            return jsonify({"error": "GEMINI_API_KEY is not set"}), 500

        image_bytes = base64.b64decode(_latest_image["image_b64"])
        prompt = _build_image_prompt(question, _latest.get("temperature"), _latest.get("humidity"))
        result = _call_gemini_with_image(prompt, image_bytes, _latest_image["mime_type"])

        now_iso = datetime.now(timezone.utc).isoformat()
        _latest.update(
            {
                "plant_result": result,
                "plant_timestamp": now_iso,
                "plant_question": question,
            }
        )
        _latest_image.update({"plant_result": result, "plant_timestamp": now_iso})

        return jsonify({"ok": True, "result": result, "timestamp": now_iso})

    except Exception as e:
        print("[analyze-image] 에러:", e)
        return jsonify({"error": str(e)}), 500


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)