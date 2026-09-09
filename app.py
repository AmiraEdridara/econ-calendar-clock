import logging
import os
import threading
import time
import traceback

from dotenv import load_dotenv
from flask import Flask, jsonify
from apify_client import ApifyClient
from datetime import datetime, timezone

load_dotenv()

logging.basicConfig(level=logging.INFO)

app = Flask(__name__)

APIFY_TOKEN = os.environ["APIFY_TOKEN"]
client = ApifyClient(APIFY_TOKEN)

REFRESH_SECONDS = 6 * 60 * 60  # refresh from Apify every 6 hours
RETRY_SECONDS = 60             # back off briefly if a refresh fails

cached_events = []
last_fetch = None
last_error = None

def fetch_events():
    global cached_events, last_fetch

    run_input = {
        "highImpactOnly": True,   # CPI, NFP, FOMC etc.
        "upcomingOnly": True,     # drop events that have already been released
        "sortBy": "date_asc",
        "dateRange": "two_weeks",  # runway so the clock always has a next event
    }

    run = client.actor("gochujang/economic-calendar-tracker").call(run_input=run_input)

    items = client.dataset(run.default_dataset_id).list_items().items

    # date_iso carries an offset (e.g. "-04:00"); normalise to naive UTC
    # so the ESP32 gets a plain "2026-09-10T13:30:00".
    cached_events = [
        {
            "name": item["title"],
            "datetime": datetime.fromisoformat(item["date_iso"])
            .astimezone(timezone.utc)
            .replace(tzinfo=None)
            .isoformat(),
        }
        for item in items
        if item.get("title") and item.get("date_iso")
    ]
    last_fetch = datetime.now(timezone.utc).replace(tzinfo=None)

def _refresh_loop():
    global last_error

    """Keep the cache warm in the background.

    The Apify run takes ~30s, which is far longer than the ESP32's HTTP
    timeout, so it must never happen inside a request.
    """
    while True:
        try:
            fetch_events()
            last_error = None
            app.logger.info("cached %d events", len(cached_events))
            time.sleep(REFRESH_SECONDS)
        except Exception:
            last_error = traceback.format_exc()
            app.logger.error("background refresh failed: %s", last_error)
            time.sleep(RETRY_SECONDS)


_refresher_started = False
_refresher_lock = threading.Lock()


@app.before_request
def _start_refresher():
    """Start the refresh thread inside the worker process.

    Starting it at import time is unreliable under gunicorn: if the app is
    imported before the fork, the thread keeps running in the parent and the
    worker that serves requests never sees the cache fill.
    """
    global _refresher_started
    if not _refresher_started:
        with _refresher_lock:
            if not _refresher_started:
                threading.Thread(target=_refresh_loop, daemon=True).start()
                _refresher_started = True

@app.route("/next-events")
def next_events():
    try:
        if last_fetch is None:
            return jsonify({"error": "warming_up",
                            "message": "No data cached yet, try again shortly."}), 503

        now = datetime.now(timezone.utc).replace(tzinfo=None)
        upcoming = [
            e for e in cached_events
            if datetime.fromisoformat(e["datetime"]) > now
        ]
        upcoming.sort(key=lambda e: e["datetime"])
        return jsonify(upcoming)
    except Exception as exc:
        app.logger.error("next-events failed: %s", traceback.format_exc())
        return jsonify({"error": type(exc).__name__, "message": str(exc)}), 500

@app.route("/status")
def status():
    return jsonify({
        "last_fetch": last_fetch.isoformat() if last_fetch else None,
        "cached_events": len(cached_events),
        "sample": cached_events[:3],
        "last_error": last_error,
    })

@app.route("/")
def home():
    return "Economic calendar API is running."

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)