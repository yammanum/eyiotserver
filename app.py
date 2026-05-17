import os
from datetime import datetime, timezone

import requests
from flask import Flask, jsonify, render_template, request

# Flask 앱 생성
app = Flask(__name__)

# Gemini API 설정 (Vercel 환경변수 사용)
API_KEY = os.getenv("GEMINI_API_KEY", "")
GEMINI_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent"

# 최신 센서 데이터를 메모리에 보관
_latest: dict = {}


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
        if not API_KEY:
            return jsonify({"error": "GEMINI_API_KEY is not set"}), 500

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

        headers = {
            "Content-Type": "application/json",
            "x-goog-api-key": API_KEY,
        }
        body = {
            "contents": [
                {
                    "parts": [
                        {"text": prompt},
                    ]
                }
            ]
        }

        res = requests.post(GEMINI_URL, headers=headers, json=body, timeout=10)
        res.raise_for_status()
        result_json = res.json()
        result = result_json["candidates"][0]["content"]["parts"][0]["text"]

        print(f"[AI 응답] {result}")

        # 최신 데이터 저장 (대시보드 표시용)
        _latest.update({
            "temperature": temp,
            "humidity": hum,
            "result": result,
            "timestamp": datetime.now(timezone.utc).isoformat(),
        })

        return jsonify({"result": result})

    except requests.RequestException as e:
        print("Gemini 요청 에러:", e)
        return jsonify({"error": "Gemini request failed", "detail": str(e)}), 502
    except (KeyError, IndexError, TypeError) as e:
        print("Gemini 응답 파싱 에러:", e)
        return jsonify({"error": "Invalid Gemini response", "detail": str(e)}), 502
    except Exception as e:
        print("에러:", e)
        return jsonify({"error": str(e)}), 500


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)