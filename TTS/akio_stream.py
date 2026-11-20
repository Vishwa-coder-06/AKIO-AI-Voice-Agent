import io
import json
import wave
import urllib.request
from urllib.parse import urlparse, parse_qs
from http.server import BaseHTTPRequestHandler, HTTPServer

import lameenc


PIPER_URL = "http://127.0.0.1:5000/synthesize"


def generate_mp3(text):

    print()
    print("TTS text:")
    print(text)

    # -----------------------------
    # Ask Piper
    # --------------------

    data = json.dumps({
        "text": text
    }).encode()

    request = urllib.request.Request(
        PIPER_URL,
        data=data,
        headers={
            "Content-Type": "application/json"
        },
        method="POST"
    )

    with urllib.request.urlopen(request) as response:
        wav_bytes = response.read()

    print("Piper WAV received:", len(wav_bytes), "bytes")

    # -----------------------------
    # Read WAV
    # -----------------------------

    wav = wave.open(io.BytesIO(wav_bytes), "rb")

    channels = wav.getnchannels()
    sample_rate = wav.getframerate()
    sample_width = wav.getsampwidth()

    print("Channels:", channels)
    print("Sample rate:", sample_rate)
    print("Sample width:", sample_width)

    if sample_width != 2:
        raise RuntimeError("Expected 16-bit PCM WAV")

    # -----------------------------
    # WAV -> MP3
    # -----------------------------

    encoder = lameenc.Encoder()

    encoder.set_bit_rate(64)
    encoder.set_in_sample_rate(sample_rate)
    encoder.set_channels(channels)
    encoder.set_quality(5)

    mp3_data = bytearray()

    while True:

        pcm = wav.readframes(8192)

        if not pcm:
            break

        encoded = encoder.encode(pcm)

        if encoded:
            mp3_data.extend(encoded)

    wav.close()

    print("MP3 generated:", len(mp3_data), "bytes")

    return bytes(mp3_data)



class AKIOHandler(BaseHTTPRequestHandler):

    def do_GET(self):

        parsed = urlparse(self.path)

        if parsed.path != "/stream":
            self.send_error(404)
            return

        # Get text from URL
        params = parse_qs(parsed.query)
        text_list = params.get("text")

        if not text_list:
            self.send_error(400, "Missing text parameter")
            return

        text = text_list[0]

        try:

            mp3_data = generate_mp3(text)

            # Send MP3 headers
            self.send_response(200)

            self.send_header(
                "Content-Type",
                "audio/mpeg"
            )

            self.send_header(
                "Content-Length",
                str(len(mp3_data))
            )

            self.send_header(
                "Connection",
                "close"
            )

            self.end_headers()

            # Send complete MP3 payload
            self.wfile.write(mp3_data)
            self.wfile.flush()

            print(
                "MP3 sent successfully:",
                len(mp3_data),
                "bytes"
            )

        except Exception as e:

            print("ERROR:", repr(e))

            # Only send an HTTP error if headers haven't been sent
            if not self.wfile.closed:
                try:
                    self.send_error(500, str(e))
                except Exception:
                    pass

    def log_message(self, format, *args):
        return


server = HTTPServer(
    ("0.0.0.0", 5001),
    AKIOHandler
)

print("================================")
print(" AKIO MP3 SERVER")
print("================================")
print("Listening on port 5001")
print("Waiting for ESP32...")
print()

server.serve_forever()
