
import io
import json
import wave
import urllib.request
from urllib.parse import urlparse, parse_qs
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import lameenc


# ==================================================
# CONFIGURATION
# ==================================================

PIPER_URL = "http://127.0.0.1:5000/synthesize"

HOST = "0.0.0.0"
PORT = 5001


# ==================================================
# GENERATE MP3
# ==================================================

def generate_mp3(text):

    print()
    print("================================")
    print("TTS TEXT:")
    print(text)
    print("================================")

    # ----------------------------------------------
    # Request WAV from Piper
    # ----------------------------------------------

    data = json.dumps({
        "text": text
    }).encode("utf-8")

    request = urllib.request.Request(
        PIPER_URL,
        data=data,
        headers={
            "Content-Type": "application/json"
        },
        method="POST"
    )

    with urllib.request.urlopen(request, timeout=120) as response:
        wav_bytes = response.read()

    print("[ PIPER ] WAV received:", len(wav_bytes), "bytes")

    # ----------------------------------------------
    # Read WAV
    # ----------------------------------------------

    with wave.open(io.BytesIO(wav_bytes), "rb") as wav:

        channels = wav.getnchannels()
        sample_rate = wav.getframerate()
        sample_width = wav.getsampwidth()
        frame_count = wav.getnframes()

        print("[ WAV ] Channels:", channels)
        print("[ WAV ] Sample rate:", sample_rate)
        print("[ WAV ] Sample width:", sample_width)
        print("[ WAV ] Frames:", frame_count)

        if sample_width != 2:
            raise RuntimeError("Expected 16-bit PCM WAV")

        if channels not in (1, 2):
            raise RuntimeError("Only mono and stereo WAV are supported")

        if frame_count == 0:
            raise RuntimeError("Piper returned empty WAV audio")

        # ------------------------------------------
        # Initialize MP3 encoder
        # ------------------------------------------

        encoder = lameenc.Encoder()

        encoder.set_bit_rate(64)
        encoder.set_in_sample_rate(sample_rate)
        encoder.set_channels(channels)
        encoder.set_quality(5)

        mp3_data = bytearray()

        # ------------------------------------------
        # Encode PCM data
        # ------------------------------------------

        while True:

            pcm = wav.readframes(8192)

            if not pcm:
                break

            encoded = encoder.encode(pcm)

            if encoded:
                mp3_data.extend(encoded)

        # ------------------------------------------
        # FLUSH ONCE AFTER THE LOOP
        # ------------------------------------------

        final_data = encoder.flush()

        if final_data:
            mp3_data.extend(final_data)

    print("[ MP3 ] Generated:", len(mp3_data), "bytes")

    if len(mp3_data) == 0:
        raise RuntimeError("MP3 encoding produced empty output")

    return bytes(mp3_data)


# ==================================================
# HTTP SERVER
# ==================================================

class AKIOHandler(BaseHTTPRequestHandler):

    protocol_version = "HTTP/1.1"

    def do_GET(self):

        parsed = urlparse(self.path)

        if parsed.path != "/stream":
            self.send_error(404, "Not found")
            return

        params = parse_qs(parsed.query)

        text_list = params.get("text")

        if not text_list or not text_list[0].strip():
            self.send_error(400, "Missing text parameter")
            return

        text = text_list[0]

        try:

            # Generate complete MP3 before sending headers
            mp3_data = generate_mp3(text)

            # ------------------------------------------
            # Send response
            # ------------------------------------------

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

            print("[ HTTP ] MP3 sent successfully")
            print("[ HTTP ] Bytes:", len(mp3_data))

        except Exception as e:

            print("[ ERROR ]", repr(e))

            try:
                self.send_error(500, "MP3 generation failed")
            except Exception:
                pass

        finally:
            self.close_connection = True

    def log_message(self, format, *args):
        return


# ==================================================
# START SERVER
# ==================================================

server = ThreadingHTTPServer(
    (HOST, PORT),
    AKIOHandler
)

server.daemon_threads = True

print("================================")
print("       AKIO MP3 SERVER")
print("================================")
print("Listening on port", PORT)
print("Waiting for ESP32...")
print()

try:
    server.serve_forever()

except KeyboardInterrupt:
    print("\n[ SERVER ] Shutting down...")

finally:
    server.server_close()
