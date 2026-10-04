#!/usr/bin/env python3
"""QuickFlip server: upload, list and watch notes. Python standard library only.

    python3 server.py                     start on port 8780 (the address to use is printed)
    python3 server.py add-device NAME     make an upload key for a person or device
    python3 server.py list-devices        show the keys
    python3 server.py remove-device NAME  take a key away

Anyone who can reach the server can watch and star notes. Uploading needs a key. A key made
with --admin can also delete anyone's notes. Everything is kept in data/ next to this file.
"""
import argparse
import json
import mimetypes
import os
import re
import secrets
import socket
import sqlite3
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import qflip

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(HERE, "static")
WEBAPP = os.path.normpath(os.path.join(HERE, "..", "web"))
MAX_UPLOAD = 2 * 1024 * 1024
ID_RE = re.compile(r"^[a-z0-9]{8}$")


class Store:
    def __init__(self, root):
        self.root = root
        self.notes = os.path.join(root, "notes")
        self.cache = os.path.join(root, "cache")
        os.makedirs(self.notes, exist_ok=True)
        os.makedirs(self.cache, exist_ok=True)
        self.lock = threading.Lock()
        self.db = sqlite3.connect(os.path.join(root, "quickflip.db"), check_same_thread=False)
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS devices (name TEXT PRIMARY KEY, key TEXT UNIQUE, admin INT);
            CREATE TABLE IF NOT EXISTS notes (id TEXT PRIMARY KEY, author TEXT, author_id TEXT,
                device TEXT, frames INT, speed INT, loop INT, created INT, uploaded INT,
                size INT, stars INT DEFAULT 0, views INT DEFAULT 0);
            CREATE TABLE IF NOT EXISTS stars (id TEXT, who TEXT, PRIMARY KEY (id, who));
        """)
        self.db.commit()

    def q(self, sql, args=()):
        with self.lock:
            cur = self.db.execute(sql, args)
            rows = cur.fetchall()
            self.db.commit()
            return rows

    def device_for(self, key):
        if not key:
            return None
        rows = self.q("SELECT name, admin FROM devices WHERE key = ?", (key,))
        return {"name": rows[0][0], "admin": bool(rows[0][1])} if rows else None

    def path(self, nid):
        return os.path.join(self.notes, nid + ".qflip")

    def add(self, data, device):
        note = qflip.Note(data)
        note.check()
        nid = "".join(secrets.choice("abcdefghijkmnpqrstuvwxyz23456789") for _ in range(8))
        with open(self.path(nid), "wb") as fp:
            fp.write(data)
        self.q("INSERT INTO notes (id, author, author_id, device, frames, speed, loop, created,"
               " uploaded, size) VALUES (?,?,?,?,?,?,?,?,?,?)",
               (nid, note.author, note.author_id, device, note.count, note.speed, note.loop,
                note.created, int(time.time()), len(data)))
        return nid

    COLS = "id, author, author_id, device, frames, speed, loop, created, uploaded, size, stars, views"

    def row(self, r):
        keys = [c.strip() for c in self.COLS.split(",")]
        return dict(zip(keys, r))

    def get(self, nid):
        rows = self.q("SELECT %s FROM notes WHERE id = ?" % self.COLS, (nid,))
        return self.row(rows[0]) if rows else None

    def list(self, sort, offset, limit, author_id=None):
        order = "stars DESC, uploaded DESC" if sort == "top" else "uploaded DESC"
        where, args = "", []
        if author_id:
            where, args = "WHERE author_id = ?", [author_id]
        rows = self.q("SELECT %s FROM notes %s ORDER BY %s LIMIT ? OFFSET ?" % (self.COLS, where, order),
                      args + [limit, offset])
        total = self.q("SELECT COUNT(*) FROM notes %s" % where, args)[0][0]
        return [self.row(r) for r in rows], total

    def delete(self, nid):
        self.q("DELETE FROM notes WHERE id = ?", (nid,))
        self.q("DELETE FROM stars WHERE id = ?", (nid,))
        for p in (self.path(nid), os.path.join(self.cache, nid + ".png"), os.path.join(self.cache, nid + ".gif")):
            if os.path.exists(p):
                os.remove(p)

    def star(self, nid, who):
        with self.lock:
            try:
                self.db.execute("INSERT INTO stars (id, who) VALUES (?, ?)", (nid, who))
                self.db.execute("UPDATE notes SET stars = stars + 1 WHERE id = ?", (nid,))
            except sqlite3.IntegrityError:
                pass
            self.db.commit()
            return self.db.execute("SELECT stars FROM notes WHERE id = ?", (nid,)).fetchone()[0]

    def picture(self, nid, kind):
        """Thumbnail PNG or animated GIF, made on first request and kept."""
        out = os.path.join(self.cache, nid + "." + kind)
        if not os.path.exists(out):
            with open(self.path(nid), "rb") as fp:
                note = qflip.Note(fp.read())
            if kind == "png":
                data = qflip.png(qflip.render(note.frame(note.thumb)))
            else:
                data = qflip.gif(note, scale=2)
            tmp = out + ".tmp%d" % threading.get_ident()
            with open(tmp, "wb") as fp:
                fp.write(data)
            os.replace(tmp, out)
        with open(out, "rb") as fp:
            return fp.read()


class Handler(BaseHTTPRequestHandler):
    server_version = "QuickFlip/1"
    store = None  # set in serve()

    def log_message(self, fmt, *args):
        if not getattr(self.server, "quiet", False):
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    # ---- replies
    def cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")

    def reply(self, code, body, ctype="application/json", cache=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if cache:
            self.send_header("Cache-Control", cache)
        self.cors()
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def fail(self, code, msg):
        self.reply(code, {"error": msg})

    def file(self, root, rel):
        path = os.path.normpath(os.path.join(root, rel))
        if not path.startswith(root + os.sep) and path != root:
            return self.fail(404, "not found")
        if os.path.isdir(path):
            path = os.path.join(path, "index.html")
        if not os.path.isfile(path):
            return self.fail(404, "not found")
        ctype = mimetypes.guess_type(path)[0] or "application/octet-stream"
        if path.endswith(".wasm"):
            ctype = "application/wasm"
        elif path.endswith(".webmanifest"):
            ctype = "application/manifest+json"
        with open(path, "rb") as fp:
            self.reply(200, fp.read(), ctype, "no-cache")

    def key(self):
        auth = self.headers.get("Authorization", "")
        return auth[7:].strip() if auth.startswith("Bearer ") else ""

    # ---- requests
    def do_OPTIONS(self):
        self.send_response(204)
        self.cors()
        self.end_headers()

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        path, qs = url.path, urllib.parse.parse_qs(url.query)
        st = self.store
        if path in ("/", "/index.html"):
            return self.file(STATIC, "index.html")
        if path == "/favicon.ico":
            return self.file(WEBAPP, "icon-192.png")
        if path.startswith("/static/"):
            return self.file(STATIC, path[len("/static/"):])
        if path == "/app":
            self.send_response(301)
            self.send_header("Location", "/app/")
            self.end_headers()
            return
        if path.startswith("/app/"):
            return self.file(WEBAPP, urllib.parse.unquote(path[len("/app/"):]) or "index.html")
        m = re.match(r"^/n/([a-z0-9]{8})$", path)
        if m:
            if not st.get(m.group(1)):
                return self.fail(404, "no such note")
            return self.file(STATIC, "view.html")
        if path == "/api/info":
            return self.reply(200, {"name": "QuickFlip", "version": 1, "key_valid": bool(st.device_for(self.key()))})
        if path == "/api/notes":
            try:
                offset = max(0, int(qs.get("offset", ["0"])[0]))
                limit = min(100, max(1, int(qs.get("limit", ["24"])[0])))
            except ValueError:
                return self.fail(400, "bad offset or limit")
            notes, total = st.list(qs.get("sort", ["new"])[0], offset, limit, qs.get("author_id", [None])[0])
            return self.reply(200, {"notes": notes, "total": total})
        m = re.match(r"^/api/notes/([a-z0-9]{8})(\.qflip|\.png|\.gif)?$", path)
        if m:
            nid, ext = m.group(1), m.group(2)
            info = st.get(nid)
            if not info:
                return self.fail(404, "no such note")
            if not ext:
                return self.reply(200, info)
            if ext == ".qflip":
                if self.command == "GET":
                    st.q("UPDATE notes SET views = views + 1 WHERE id = ?", (nid,))
                with open(st.path(nid), "rb") as fp:
                    return self.reply(200, fp.read(), "application/octet-stream", "public, max-age=86400")
            kind = ext[1:]
            return self.reply(200, st.picture(nid, kind), "image/" + kind, "public, max-age=86400")
        self.fail(404, "not found")

    def do_POST(self):
        path = urllib.parse.urlsplit(self.path).path
        st = self.store
        if path == "/api/notes":
            dev = st.device_for(self.key())
            if not dev:
                return self.fail(401, "an upload key is needed (ask whoever runs the server)")
            try:
                n = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                n = -1
            if n <= 0 or n > MAX_UPLOAD:
                return self.fail(413, "the note must be under %d KB" % (MAX_UPLOAD // 1024))
            data = self.rfile.read(n)
            try:
                nid = st.add(data, dev["name"])
            except (qflip.BadFile, IndexError, ValueError) as e:
                return self.fail(400, "not a valid note: %s" % e)
            return self.reply(201, {"id": nid, "url": "/n/" + nid})
        m = re.match(r"^/api/notes/([a-z0-9]{8})/star$", path)
        if m:
            nid = m.group(1)
            if not st.get(nid):
                return self.fail(404, "no such note")
            dev = st.device_for(self.key())
            who = "dev:" + dev["name"] if dev else "ip:" + self.client_address[0]
            return self.reply(200, {"stars": st.star(nid, who)})
        self.fail(404, "not found")

    def do_DELETE(self):
        path = urllib.parse.urlsplit(self.path).path
        st = self.store
        m = re.match(r"^/api/notes/([a-z0-9]{8})$", path)
        if not m:
            return self.fail(404, "not found")
        info = st.get(m.group(1))
        if not info:
            return self.fail(404, "no such note")
        dev = st.device_for(self.key())
        if not dev or not (dev["admin"] or dev["name"] == info["device"]):
            return self.fail(403, "only the device that uploaded it (or an admin key) can delete it")
        st.delete(info["id"])
        self.reply(200, {"deleted": info["id"]})


def lan_address():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


def serve(root, host, port, quiet=False):
    Handler.store = Store(root)
    httpd = ThreadingHTTPServer((host, port), Handler)
    httpd.quiet = quiet
    return httpd


def main():
    ap = argparse.ArgumentParser(description="QuickFlip server")
    ap.add_argument("command", nargs="?", default="run",
                    choices=["run", "add-device", "list-devices", "remove-device"])
    ap.add_argument("name", nargs="?")
    ap.add_argument("--admin", action="store_true", help="with add-device: the key can delete any note")
    ap.add_argument("--port", type=int, default=8780)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    a = ap.parse_args()
    os.makedirs(a.data, exist_ok=True)
    if a.command == "add-device":
        if not a.name:
            sys.exit("give a name: python3 server.py add-device NAME")
        st = Store(a.data)
        key = secrets.token_urlsafe(12)
        try:
            st.q("INSERT INTO devices (name, key, admin) VALUES (?, ?, ?)", (a.name, key, int(a.admin)))
        except sqlite3.IntegrityError:
            sys.exit("there is already a device called %r (remove-device it first)" % a.name)
        print("Upload key for %s%s: %s" % (a.name, " (admin)" if a.admin else "", key))
        return
    if a.command == "list-devices":
        for name, key, admin in Store(a.data).q("SELECT name, key, admin FROM devices ORDER BY name"):
            print("%-20s %s%s" % (name, key, "  admin" if admin else ""))
        return
    if a.command == "remove-device":
        Store(a.data).q("DELETE FROM devices WHERE name = ?", (a.name,))
        print("removed", a.name)
        return
    httpd = serve(a.data, a.host, a.port)
    ip = lan_address()
    print("QuickFlip server running.")
    print("  Gallery:      http://%s:%d/" % (ip, a.port))
    print("  Drawing app:  http://%s:%d/app/" % (ip, a.port))
    print("  Server address for devices and the app settings: http://%s:%d" % (ip, a.port))
    print("Stop with Ctrl+C.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
