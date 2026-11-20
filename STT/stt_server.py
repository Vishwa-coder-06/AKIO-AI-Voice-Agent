
from flask import Flask, request, jsonify
from faster_whisper import WhisperModel
import tempfile
import os

app = Flask(__name__)

print("Loading Whisper model...")

model = WhisperModel(
    "base.en",
    device="cpu",
    compute_type="int8"
)

print("AKIO STT Server Ready!")


@app.route("/transcribe", methods=["POST"])
def transcribe():

    # Receive raw WAV data from ESP32
    audio_bytes = request.get_data()

    if not audio_bytes:
        return jsonify({"error": "No audio received"}), 400

    # Save received audio temporarily
    with tempfile.NamedTemporaryFile(
        suffix=".wav",
        delete=False
    ) as temp:

        temp.write(audio_bytes)
        audio_path = temp.name

    try:
        print("Audio received:", len(audio_bytes), "bytes")
        print("Starting transcription...")

        segments, info = model.transcribe(
            audio_path,
            beam_size=1,
            vad_filter=True
        )

        recognized_text = " ".join(
            segment.text.strip()
            for segment in segments
        )

        print("Recognized:", recognized_text)

        return jsonify({
            "text": recognized_text,
            "language": info.language
        })

    except Exception as e:
        print("STT Error:", str(e))

        return jsonify({
            "error": str(e)
        }), 500

    finally:
        if os.path.exists(audio_path):
            os.remove(audio_path)



if __name__ == "__main__":
    app.run(
        host="0.0.0.0",
        port=5002,
        debug=False
    )
