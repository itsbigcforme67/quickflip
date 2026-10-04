"""Server tests: the Python reader must draw exactly what the C core draws, and the web API
must accept, list, picture, star and delete notes. Run through tests/run.sh."""
import json
import os
import sys
import tempfile
import threading
import urllib.request
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "server"))
import qflip  # noqa: E402
import server  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        print("FAIL", what)
        fails += 1


sample = sys.argv[1]
data = open(sample, "rb").read()
note = qflip.Note(data)
note.check()
check(note.count == 40 and note.speed == 6 and note.loop == 0 and note.thumb == 3, "header")
check(note.author == "Tester é" and note.author_id == "0102030405060708", "author")
frames = open(sample + ".frames", "rb").read()
size = qflip.W * qflip.H
for i, f in enumerate(note.frames()):
    check(qflip.render(f) == frames[i * size:(i + 1) * size], "frame %d matches the C core" % i)
check(qflip.png(qflip.render(note.frame(0))).startswith(b"\x89PNG"), "png")
g = qflip.gif(note)
check(g.startswith(b"GIF89a") and g.endswith(b";"), "gif")
try:
    from PIL import Image  # optional: checks the GIF really decodes to the right pictures
    import io
    im = Image.open(io.BytesIO(g))
    for i, f in enumerate(note.frames()):
        im.seek(i)
        want = b"".join(bytes(qflip.PALETTE[c]) for c in qflip.render(f))
        check(im.convert("RGB").tobytes() == want, "gif frame %d" % i)
    print("GIF checked with Pillow")
except ImportError:
    print("(Pillow not installed: GIF pictures not checked)")

for bad in (b"", b"QFLP\x02", data[:40], data[:-3], b"QFLP\x01META\xff\xff\xff\xff"):
    try:
        qflip.Note(bad).check()
        check(False, "damaged file accepted")
    except qflip.BadFile:
        pass

# ---- the web API
root = tempfile.mkdtemp()
httpd = server.serve(root, "127.0.0.1", 0, quiet=True)
threading.Thread(target=httpd.serve_forever, daemon=True).start()
base = "http://127.0.0.1:%d" % httpd.server_address[1]
st = server.Handler.store
st.q("INSERT INTO devices VALUES ('cyd', 'k1', 0), ('boss', 'k2', 1), ('other', 'k3', 0)")


def call(method, path, body=None, key=None):
    req = urllib.request.Request(base + path, data=body, method=method)
    if key:
        req.add_header("Authorization", "Bearer " + key)
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, r.read(), r.headers
    except urllib.error.HTTPError as e:
        return e.code, e.read(), e.headers


code, body, _ = call("POST", "/api/notes", data)
check(code == 401, "upload without a key refused")
code, body, _ = call("POST", "/api/notes", b"QFLP\x01junk", key="k1")
check(code == 400, "junk upload refused")
code, body, hdr = call("POST", "/api/notes", data, key="k1")
check(code == 201, "upload")
nid = json.loads(body)["id"]
check(hdr["Access-Control-Allow-Origin"] == "*", "cors")
code, body, _ = call("GET", "/api/notes")
lst = json.loads(body)
check(lst["total"] == 1 and lst["notes"][0]["id"] == nid and lst["notes"][0]["frames"] == 40, "list")
code, body, _ = call("GET", "/api/notes/%s.qflip" % nid)
check(body == data, "download")
code, body, _ = call("GET", "/api/notes/%s.png" % nid)
check(code == 200 and body.startswith(b"\x89PNG"), "thumbnail")
code, body, hdr = call("GET", "/api/notes/%s.gif" % nid)
check(code == 200 and body.startswith(b"GIF89a"), "gif")
check(json.loads(call("POST", "/api/notes/%s/star" % nid)[1])["stars"] == 1, "star")
check(json.loads(call("POST", "/api/notes/%s/star" % nid)[1])["stars"] == 1, "one star each")
check(json.loads(call("POST", "/api/notes/%s/star" % nid, key="k1")[1])["stars"] == 2, "star by device")
check(json.loads(call("GET", "/api/notes/" + nid)[1])["views"] == 1, "views")
check(call("GET", "/n/" + nid)[0] == 200, "viewer page")
check(call("GET", "/")[0] == 200, "gallery page")
check(call("GET", "/app/")[0] == 200, "drawing app")
check(call("GET", "/app/core/qflip.wasm")[2]["Content-Type"] == "application/wasm", "wasm type")
check(call("GET", "/app/../server/server.py")[0] == 404, "no files outside the app")
check(call("GET", "/api/info", key="k1")[1].find(b'"key_valid": true') > 0, "key check")
check(call("DELETE", "/api/notes/" + nid, key="k3")[0] == 403, "other device cannot delete")
check(call("DELETE", "/api/notes/" + nid, key="k2")[0] == 200, "admin deletes")
check(json.loads(call("GET", "/api/notes")[1])["total"] == 0, "gone")
httpd.shutdown()

print("server: %d failed" % fails if fails else "server: ALL TESTS PASSED")
sys.exit(1 if fails else 0)
