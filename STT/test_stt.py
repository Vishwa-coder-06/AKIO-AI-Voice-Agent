
import requests

url = "http://127.0.0.1:5002/transcribe"

with open("recording.wav", "rb") as audio:
    response = requests.post(
        url,
        files={
            "audio": (
                "recording.wav",
                audio,
                "audio/wav"
            )
        }
    )

print("STT Response:", response.json())
