import os
from datetime import datetime, timezone

from google import genai
from flask import Flask, jsonify, render_template, request

# Flask 앱 생성
app = Flask(__name__)

# Gemini API 설정 (Vercel 환경변수 사용)
API_KEY = os.getenv("GEMINI_API_KEY", "")
GEMINI_MODEL = "gemini-2.5-flash"

# 최신 센서 데이터를 메모리에 보관
_latest: dict = {}


def _decide_motors(temp: float, hum: float) -> tuple[bool, bool]:
    # 간단한 안전 규칙: 고온이면 팬 ON, 저습이면 펌프 ON
    fan_on = temp >= 28.0
    pump_on = hum <= 45.0
    return fan_on, pump_on


def _build_fallback_result(temp: float, hum: float, reason: str) -> str:
    fan_on, pump_on = _decide_motors(temp, hum)
    fan = "on" if fan_on else "off"
    pump = "on" if pump_on else "off"
    return (
        "Gemini 호출에 실패하여 규칙 기반 제어를 사용합니다.\n"
        f"측정값: temperature={temp}, humidity={hum}\n"
        f"사유: {reason}\n"
        f"{{\"fan_motor\": \"{fan}\", \"water_pump\": \"{pump}\"}}"
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


@app.route("/", methods=["GET"])
def index():
    return render_template("index.html")


@app.route("/api/latest", methods=["GET"])
def api_latest():
    return jsonify(_latest)


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
            f"현재 온도 {temp}도, 습도 {hum}%입니다. "
            "잉글리쉬 라벤더 생육 환경에 적합한지 알려주세요. "
            "습도와 온도에 따라 선풍기 모터를 켜고 물펌프모터를 켜야할지 알려주세요. "
            "Esp32로 제어할 수 있도록 선풍기 모터와 물펌프 모터의 상태를 JSON 형태로 알려주세요."
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


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)