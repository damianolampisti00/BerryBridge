"""igview: opens Instagram view-once photos and videos for Berry Bridge.

Beeper's Instagram bridge cannot receive view-once ("raven") media: Instagram
only hands them to its mobile app, so the bridge shows "This image can only be
viewed once. Use the Instagram app to view." This service talks to Instagram's
mobile API (instagrapi), with the account's existing session, and serves such
a media exactly once:

    GET /v1/open?ts=<unix seconds of the notice>      Authorization: Bearer <token>

finds the unseen view-once item closest to that time (within MATCH_WINDOW_S),
streams it to the caller and marks it seen on Instagram. Nothing is written to
disk and nothing is cached: once opened it is gone, as on the official app.

    GET /v1/pending                                   (metadata only, for checks)

and sends them, as the Android app does (upload marked ephemeral, then a
raven_attachment with view_mode "once"):

    POST /v1/send_once?username=<the other person>   body: the photo (JPEG/PNG)

into the one-to-one thread with that user (it must exist already).

    GET /v1/wa/...                                    (WhatsApp, same token)

is passed on to waviewonce (WaCalls' cmd/waviewonce, on WAVIEWONCE_ADDR), which
does the same for WhatsApp view-once media, so that Berry Bridge needs one
address and one token for both.

Setup: data/settings.json (instagrapi session, created by `login_from_feather`),
data/token (generated on first start). Listens on 127.0.0.1:8103.
"""
import glob
import io
import json
import logging
import os
import random
import secrets
import threading
import time
import urllib.error
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from instagrapi import Client
from instagrapi.exceptions import ChallengeRequired, LoginRequired
from PIL import Image, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
SETTINGS = os.path.join(DATA, "settings.json")
ACCOUNT = os.path.join(DATA, "account")   # the Instagram username this service is for
ADDR = ("127.0.0.1", int(os.environ.get("IGVIEW_PORT", "8103")))
WAVIEWONCE = "http://" + os.environ.get("WAVIEWONCE_ADDR", "127.0.0.1:8104")
MATCH_WINDOW_S = 300        # a notice and its Instagram item are a few seconds apart
THREADS_TO_SCAN = 20
MAX_MEDIA_BYTES = 40 << 20
MAX_FAILURES = 10           # wrong tokens per address before a lockout
LOCKOUT_S = 15 * 60
MAX_UPLOAD_BYTES = 15 << 20

log = logging.getLogger("igview")
_lock = threading.Lock()    # one Instagram request at a time
_client = None
_failures = {}
_threads = {}               # username -> Direct thread id


def token():
    path = os.path.join(DATA, "token")
    if not os.path.exists(path):
        with open(path, "w") as f:
            f.write(secrets.token_urlsafe(24) + "\n")
        os.chmod(path, 0o600)
        log.info("new access token saved in %s", path)
    return open(path).read().strip()


def client():
    global _client
    if _client is None:
        c = Client()
        c.delay_range = [1, 2]
        c.load_settings(SETTINGS)
        c.set_locale("it_IT")
        c.set_timezone_offset(2 * 3600)
        _client = c
    return _client


def login_from_feather():
    """(Re)creates the session from the newest Feather browser session that is
    signed in to Instagram (its sessionid cookie): no password involved."""
    best = None
    for f in glob.glob(os.path.expanduser("~/server/sessions/*.json")):
        ck = {c["name"]: c["value"] for c in json.load(open(f)).get("cookies", [])
              if "instagram.com" in c.get("domain", "")}
        if "sessionid" in ck and (best is None or os.path.getmtime(f) > best[0]):
            best = (os.path.getmtime(f), ck["sessionid"])
    if not best:
        raise RuntimeError("no Instagram login in Feather")
    c = Client()
    c.delay_range = [1, 2]
    if os.path.exists(SETTINGS):
        c.load_settings(SETTINGS)
    c.set_locale("it_IT")
    c.set_timezone_offset(2 * 3600)
    c.login_by_sessionid(best[1])
    # Feather may be signed in to another account by now: never switch.
    who = c.account_info().username
    if os.path.exists(ACCOUNT) and open(ACCOUNT).read().strip() != who:
        raise RuntimeError(f"Feather is signed in to {who}, not to this service's account")
    with open(ACCOUNT, "w") as f:
        f.write(who + "\n")
    c.dump_settings(SETTINGS)
    os.chmod(SETTINGS, 0o600)


def unseen_items():
    """Unseen view-once items in the most recent threads (raw API dicts)."""
    c = client()
    out = []
    for t in c.direct_threads(amount=THREADS_TO_SCAN):
        if not any(getattr(m, "item_type", "") == "raven_media" for m in (t.messages or [])):
            continue
        raw = c.private_request(f"direct_v2/threads/{t.id}/",
                                params={"visual_message_return_type": "unseen", "direction": "older", "limit": "20"})
        for it in raw.get("thread", {}).get("items", []):
            vm = it.get("visual_media") or {}
            media = vm.get("media") or {}
            if it.get("item_type") != "raven_media" or it.get("is_sent_by_viewer"):
                continue
            if not (media.get("image_versions2") or media.get("video_versions")):
                continue  # already seen (no media left) or expired
            out.append({"thread_id": t.id, "item": it, "ts": int(it.get("timestamp", 0)) / 1e6})
    return out


def media_url(item):
    media = (item.get("visual_media") or {}).get("media") or {}
    if media.get("video_versions"):
        v = sorted(media["video_versions"], key=lambda x: x.get("width", 0))
        # The phone's screen is 720 px: the smallest version at least that wide.
        pick = next((x for x in v if x.get("width", 0) >= 720), v[-1])
        return pick["url"], "video"
    cands = sorted(media["image_versions2"]["candidates"], key=lambda x: x.get("width", 0))
    pick = next((x for x in cands if x.get("width", 0) >= 1080), cands[-1])
    return pick["url"], "image"


def mark_seen(thread_id, item_id):
    """What the app sends after a view-once media was shown."""
    c = client()
    ctx = c.generate_mutation_token()
    c.private_request(f"direct_v2/visual_threads/{thread_id}/item_seen/",
                      data={"item_ids": f"[{item_id}]", "target_item_type": "raven_media",
                            "_uuid": c.uuid, "client_context": ctx, "offline_threading_id": ctx},
                      with_signature=False)


def thread_with(username):
    """The one-to-one Direct thread with this user (cached)."""
    if username in _threads:
        return _threads[username]
    c = client()
    uid = int(c.user_id_from_username(username))
    t = c.direct_thread_by_participants([uid])
    tid = t.get("thread_v2_id") or t.get("thread_id")
    if not tid and isinstance(c.last_json, dict):
        t = c.last_json.get("thread") or {}
        tid = t.get("thread_v2_id") or t.get("thread_id")
    if tid:
        _threads[username] = str(tid)
    return _threads.get(username)


def as_jpeg(data):
    """The photo as the app sends it: upright, at most 1080x1920, JPEG."""
    img = ImageOps.exif_transpose(Image.open(io.BytesIO(data))).convert("RGB")
    img.thumbnail((1080, 1920))
    out = io.BytesIO()
    img.save(out, "JPEG", quality=88)
    return out.getvalue(), img.size


def send_once(thread_id, jpeg, size):
    """Upload marked ephemeral (0 once, 1 replayable, 2 permanent), then the
    raven_attachment broadcast with view_mode "once"."""
    import requests

    c = client()
    entity = f"fb_uploader_{int(time.time() * 1000)}"
    headers = c._messenger_rupload_headers({"image_type": "FILE_ATTACHMENT",
                                            "ephemeral_media_view_mode": "0", "ig_raven_metadata": "{}"})
    headers.update({"content-type": "application/octet-stream", "offset": "0",
                    "x-entity-length": str(len(jpeg)), "x-entity-name": entity, "x-entity-type": "image/jpeg"})
    r = requests.post(f"https://rupload.facebook.com/messenger_image/{entity}", data=jpeg, headers=headers, timeout=120)
    if r.status_code != 200:
        raise RuntimeError(f"upload failed: HTTP {r.status_code}")
    media_id = int(r.json()["media_id"])
    w, h = size
    token = c.generate_mutation_token()
    data = {
        "recipient_users": "[]", "view_mode": "once", "has_camera_metadata": "1", "camera_entry_point": "3",
        "thread_ids": json.dumps([str(thread_id)]), "reshare_mode": "allow_reshare",
        "original_media_type": "1", "send_attribution": "direct_composer", "client_context": token,
        "camera_session_id": str(uuid.uuid4()), "attachment_fbid": str(media_id),
        "include_e2ee_mentioned_user_list": "1", "hide_from_profile_grid": "false", "timezone_offset": "0",
        "client_shared_at": str(int(time.time())), "configure_mode": "2", "source_type": "4",
        "camera_position": "back", "_uid": str(c.user_id), "device_id": c.android_device_id,
        "composition_id": str(uuid.uuid4()), "mutation_token": token, "_uuid": c.uuid,
        "creation_surface": "camera", "has_ig_camera_edits": "false", "capture_type": "normal",
        "audience": "default", "upload_id": str(random.randint(10**11, 10**12 - 1)),
        "client_timestamp": str(int(time.time())),
        "media_transformation_info": json.dumps({"width": str(w), "height": str(h), "x_transform": "0",
                                                 "y_transform": "0", "zoom": "1.0", "rotation": "0.0",
                                                 "background_coverage": "0.0"}),
        "edits": {"filter_type": 0, "filter_strength": 1.0},
        "extra": {"source_width": w, "source_height": h},
        "device": {"manufacturer": "Google", "model": "sdk_gphone_arm64", "android_version": 30,
                   "android_release": "11"},
    }
    res = c.private_request("direct_v2/threads/broadcast/raven_attachment/",
                            data=c.with_default_data(data), with_signature=True)
    if res.get("status") != "ok":
        raise RuntimeError(f"broadcast: {res.get('status')}")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log.info("%s %s", self.address_string(), fmt % args)

    def address_string(self):
        return self.headers.get("CF-Connecting-IP") or self.client_address[0]

    def reply(self, code, obj=None, body=None, ctype="application/json"):
        data = body if body is not None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def authorized(self):
        ip = self.address_string()
        f = _failures.get(ip)
        if f and f["until"] > time.time():
            self.reply(429, {"error": "too many attempts"})
            return False
        if self.headers.get("Authorization", "") == "Bearer " + TOKEN:
            _failures.pop(ip, None)
            return True
        f = f if f and time.time() - f["since"] < LOCKOUT_S else {"n": 0, "since": time.time(), "until": 0}
        f["n"] += 1
        if f["n"] >= MAX_FAILURES:
            f["until"] = time.time() + LOCKOUT_S
        _failures[ip] = f
        self.reply(401, {"error": "invalid token"})
        return False

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/health":
            return self.reply(200, {"ok": True})
        if u.path.startswith("/v1/wa/qr/"):   # linking page, guarded by its own secret path
            return self.to_waviewonce()
        if not self.authorized():
            return
        if u.path.startswith("/v1/wa/"):
            return self.to_waviewonce()
        q = parse_qs(u.query)
        try:
            with _lock:
                if u.path == "/v1/pending":
                    items = unseen_items()
                    return self.reply(200, [{"ts": int(i["ts"]),
                                             "kind": "video" if (i["item"]["visual_media"]["media"].get("video_versions")) else "image",
                                             "view_mode": i["item"]["visual_media"].get("view_mode")} for i in items])
                if u.path == "/v1/open":
                    ts = float(q.get("ts", ["0"])[0])
                    items = [i for i in unseen_items() if abs(i["ts"] - ts) <= MATCH_WINDOW_S]
                    if not items:
                        return self.reply(404, {"error": "not found: already viewed, expired, or not a view-once media"})
                    best = min(items, key=lambda i: abs(i["ts"] - ts))
                    url, kind = media_url(best["item"])
                    with urllib.request.urlopen(url, timeout=30) as r:
                        data = r.read(MAX_MEDIA_BYTES + 1)
                    if len(data) > MAX_MEDIA_BYTES:
                        return self.reply(413, {"error": "media too large"})
                    self.reply(200, body=data, ctype="video/mp4" if kind == "video" else "image/jpeg")
                    try:
                        mark_seen(best["thread_id"], best["item"]["item_id"])
                        log.info("served a view-once %s and marked it seen", kind)
                    except Exception as e:  # shown anyway; the app on another phone would still offer it
                        log.warning("mark seen failed: %s", type(e).__name__)
                    return
            return self.reply(404, {"error": "unknown path"})
        except (ChallengeRequired, LoginRequired) as e:
            return self.instagram_check(e)
        except Exception as e:
            log.exception("request failed")
            return self.reply(500, {"error": type(e).__name__})

    def to_waviewonce(self):
        req = urllib.request.Request(WAVIEWONCE + self.path, headers={"Authorization": "Bearer " + TOKEN})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return self.reply(r.status, body=r.read(MAX_MEDIA_BYTES + 1), ctype=r.headers.get("Content-Type", "application/json"))
        except urllib.error.HTTPError as e:
            return self.reply(e.code, body=e.read(), ctype=e.headers.get("Content-Type", "application/json"))
        except OSError:
            return self.reply(503, {"error": "waviewonce", "message": "Servizio WhatsApp a visualizzazione singola non attivo"})

    def do_POST(self):
        u = urlparse(self.path)
        if not self.authorized():
            return
        if u.path != "/v1/send_once":
            return self.reply(404, {"error": "unknown path"})
        username = parse_qs(u.query).get("username", [""])[0].strip().lstrip("@")
        size = int(self.headers.get("Content-Length") or 0)
        if not username or size <= 0 or size > MAX_UPLOAD_BYTES:
            return self.reply(400, {"error": "username and a photo (up to 15 MB) are needed"})
        body = self.rfile.read(size)
        try:
            jpeg, dims = as_jpeg(body)
        except Exception:
            return self.reply(400, {"error": "not a photo"})
        try:
            with _lock:
                tid = thread_with(username)
                if not tid:
                    return self.reply(404, {"error": "no_thread", "message": "Nessuna chat con questo utente"})
                send_once(tid, jpeg, dims)
            log.info("sent a view-once photo")
            return self.reply(200, {"ok": True})
        except (ChallengeRequired, LoginRequired) as e:
            return self.instagram_check(e)
        except Exception as e:
            log.exception("send failed")
            return self.reply(500, {"error": type(e).__name__})

    def instagram_check(self, e):
        global _client
        _client = None
        log.warning("Instagram wants a check: %s", type(e).__name__)
        return self.reply(503, {"error": "instagram_check",
                                "message": "Instagram chiede una verifica: apri instagram.com in Feather e conferma"})


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    os.makedirs(DATA, exist_ok=True)
    os.chmod(DATA, 0o700)
    TOKEN = token()
    if not os.path.exists(SETTINGS):
        login_from_feather()
    if not os.path.exists(ACCOUNT):
        with open(ACCOUNT, "w") as f:
            f.write(client().account_info().username + "\n")
    log.info("igview listening on %s:%d", *ADDR)
    ThreadingHTTPServer(ADDR, Handler).serve_forever()
