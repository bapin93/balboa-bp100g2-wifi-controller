#!/usr/bin/env python3
"""Mock hot tub for developing the web app without the ESP32.

Serves firmware/data/ uncached (edit, then refresh) and fakes the firmware's
REST API and WebSocket state push with a simulated spa behind them. Standard
library only.

    python3 firmware/tools/mock_spa.py            # http://127.0.0.1:8090
    python3 firmware/tools/mock_spa.py --host 0.0.0.0 --reminder 4 --pumps 2,1

Force any state field while it runs:

    curl -X POST localhost:8090/mock -d '{"reminder": 10, "hold": true}'

The JSON shapes mirror SpaController::writeStateJson and friends in
src/balboa/spa.cpp; keep them in sync when the firmware changes.
"""

import argparse
import base64
import calendar
import hashlib
import json
import math
import mimetypes
import random
import struct
import threading
import time
import urllib.parse
import urllib.request
from datetime import date, datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

DATA_DIR = Path(__file__).resolve().parent.parent / "data"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
SPEED_NAMES = ["Off", "Low", "High"]
PRESS_LATENCY_S = 0.6  # how long the "spa" takes to react to a button press
# Plans use realistic physics (src/scheduler/heat_planner.cpp) even though the simulated water heats fast.
PLAN_HEATER_RATE, PLAN_LOSS, PLAN_STEP = 6.0, 0.012, 900


class MockSpa:
    def __init__(self, args):
        self.lock = threading.Lock()
        self.pump_speeds = [int(x) for x in args.pumps.split(",")] + [0, 0]
        self.heat_holds_pump = not args.no_heat_pump
        self.heat_rate = args.heat_rate  # F per real second while heating
        self.state = {
            "waterTemp": float(args.water),
            "setTemp": 100.0,
            "jet1Speed": 0,
            "jet2Speed": 0,
            "blower": False,
            "light": False,
            "heatMode": 0,
            "heating": False,
            "heatPending": False,
            "priming": False,
            "tempRangeHigh": True,
            "hold": False,
            "holdMinutes": 0,
            "reminder": args.reminder,
            "activeFilterCycle": 0,
        }
        self.filters = {
            "cycle1Start": 2, "cycle1StartMinute": 0, "cycle1Duration": 3, "cycle1DurationMinute": 0,
            "cycle2Enabled": True, "cycle2Start": 14, "cycle2StartMinute": 0, "cycle2Duration": 3,
            "cycle2DurationMinute": 0,
        }
        self.filter_seq = 1
        self.pump_cmd = {"active": False, "pump": 0, "target": 0, "seq": 0, "ok": True, "message": ""}
        self.pump_cmd_internal = None
        self.spa_info_seq = 1
        self.fault_seq = 0
        self.fault_reading_until = 0.0
        self.faults = [] if args.no_faults else [
            {"entry": 0, "code": 16, "daysAgo": 6, "hour": 14, "minute": 30, "setTemp": 100, "sensorA": 98, "sensorB": 97},
            {"entry": 1, "code": 37, "daysAgo": 1, "hour": 9, "minute": 5, "setTemp": 100, "sensorA": 99, "sensorB": 99},
        ]
        self.config = {
            "timezone": "MST7MDT,M3.2.0/2,M11.1.0/2", "authEnabled": False, "authPasswordSet": False,
            "capabilities": {"setTime": True, "hideJet2": False},
            "location": dict(zip(("lat", "lon"), map(float, args.location.split(",")))) if args.location else None,
            # A sample TOU plan so the cost views have something to show.
            "tou": {"peakStart": 17, "peakEnd": 21, "weekendsOffPeak": True, "holidays": "federal10",
                    "rates": {"summerStartMonth": 6, "summerEndMonth": 9, "summerPeak": 0.21277,
                              "summerOffPeak": 0.07884, "winterPeak": 0.18331, "winterOffPeak": 0.06792}},
            "power": {"heaterKw": 5.5, "pump1LowKw": 0.5, "pump1HighKw": 1.8, "pump2Kw": 1.5, "standbyW": 30,
                      "tubGallons": 200},
            "smart": {"minTemp": 90, "auto": True, "readyTemp": 102},
        }
        self.water_unknown = False  # like a real spa between pump runs: true temp simulated, not reported
        self.last_water = None      # last reported reading and when, as the firmware keeps it
        self.last_water_at = 0
        self.range_set_temp = {True: 100.0, False: 80.0}  # set temp remembered per range
        self.maint = self._seed_maintenance(args.no_filters)
        self.water = self._seed_water(args.no_filters)
        self.capture_active = False
        self.capture_lines = []
        self.outside_daily, self.outside_hourly = self._fetch_outside()
        self.runtime = self._fake_history()
        self.hours = self._fake_hours()
        self.pending = []  # (due_time, fn) delayed spa reactions
        self.last_tick = time.monotonic()
        self.hold_until = 0.0
        self.soak = None    # mirrors HeatController's soak, plus the plan it follows
        self.manual = None
        self.auto_plan = None  # {"start", "steps"} while Auto runs

    # ── maintenance (mirrors src/storage/maintenance.cpp) ──

    def _seed_maintenance(self, empty):
        if empty:
            return {"nextId": 1, "filters": [], "tasks": [], "tasksSeeded": True, "events": []}
        now, day = int(time.time()), 86400
        a = {"id": 1, "name": "Blue", "createdAt": now - 60 * day, "cleanedAt": now - 9 * day,
             "pumpSec": 410 * 3600, "pumpSecSinceClean": 63 * 3600,
             "stints": [[now - 60 * day, now - 30 * day], [now - 9 * day, 0]]}
        b = {"id": 2, "name": "Gray", "createdAt": now - 60 * day, "cleanedAt": now - 8 * day,
             "pumpSec": 220 * 3600, "pumpSecSinceClean": 0, "stints": [[now - 30 * day, now - 9 * day]]}
        events = [(a, now - 60 * day, "added"), (b, now - 60 * day, "added"), (a, now - 60 * day, "installed"),
                  (a, now - 30 * day, "removed"), (b, now - 30 * day, "installed"), (b, now - 9 * day, "removed"),
                  (a, now - 9 * day, "cleaned"), (a, now - 9 * day, "installed"), (b, now - 8 * day, "cleaned")]
        tasks = [{"id": 3, "name": "Drain and refill", "intervalDays": 90, "createdAt": now - 60 * day,
                  "lastDone": now - 80 * day},
                 {"id": 4, "name": "Clean the cover", "intervalDays": 30, "createdAt": now - 60 * day,
                  "lastDone": now - 35 * day},
                 {"id": 5, "name": "Deep-clean filters", "intervalDays": 90, "createdAt": now - 60 * day,
                  "lastDone": 0}]
        events += [(tasks[0], now - 80 * day, "done"), (tasks[1], now - 35 * day, "done")]
        return {"nextId": 6, "filters": [a, b], "tasks": tasks, "tasksSeeded": True,
                "events": [{"ts": ts, "itemId": f["id"], "name": f["name"], "type": t} for f, ts, t in events]}

    def maintenance_json(self):
        with self.lock:
            filters = [{**f, "pumpSec": int(f["pumpSec"]), "pumpSecSinceClean": int(f["pumpSecSinceClean"]),
                        "inTub": bool(f["stints"]) and f["stints"][-1][1] == 0} for f in self.maint["filters"]]
            return {**self.maint, "filters": filters, "now": int(time.time())}

    def maintenance_action(self, body):
        with self.lock:
            m, now = self.maint, int(time.time())
            ts = int(body.get("ts", now))
            if ts > now + 600:
                return "date must not be in the future"
            find = {f["id"]: f for f in m["filters"]}
            in_tub = lambda f: bool(f["stints"]) and f["stints"][-1][1] == 0

            def event(f, kind, when):
                m["events"] = (m["events"] + [{"ts": when, "itemId": f["id"], "name": f["name"], "type": kind}])[-40:]

            action, fid = body.get("action"), body.get("id")
            f = find.get(fid)
            task = next((t for t in m["tasks"] if t["id"] == fid), None)
            days = body.get("intervalDays", 0)
            name = str(body.get("name", "")).strip()
            if action in ("addTask", "editTask"):
                if not name or len(name) > 24:
                    return "give it a name (24 characters max)"
                if not isinstance(days, int) or not 1 <= days <= 730:
                    return "repeat every 1 to 730 days"
            if action == "addTask":
                m["tasks"].append({"id": m["nextId"], "name": name, "intervalDays": days, "createdAt": now,
                                   "lastDone": 0})
                m["nextId"] += 1
                return None
            if action == "editTask" and task:
                task.update(name=name, intervalDays=days)
                return None
            if action == "deleteTask" and task:
                m["tasks"].remove(task)
                return None
            if action == "taskDone" and task:
                task["lastDone"] = max(task["lastDone"], ts)
                event(task, "done", ts)
                return None
            if action in ("editTask", "deleteTask", "taskDone"):
                return "task not found"
            if action == "add":
                name = str(body.get("name", "")).strip()
                if not name or len(name) > 24:
                    return "give the filter a name (24 characters max)"
                f = {"id": m["nextId"], "name": name, "createdAt": ts, "cleanedAt": 0, "pumpSec": 0,
                     "pumpSecSinceClean": 0, "stints": []}
                m["nextId"] += 1
                m["filters"].append(f)
                event(f, "added", ts)
            elif action == "rename" and f:
                name = str(body.get("name", "")).strip()
                if not name or len(name) > 24:
                    return "give the filter a name (24 characters max)"
                f["name"] = name
            elif action == "delete" and f:
                event(f, "deleted", now)
                m["filters"].remove(f)
            elif action == "install" and f:
                if in_tub(f):
                    return "that filter is already in the tub"
                f["stints"].append([ts, 0])
                event(f, "installed", ts)
            elif action == "remove" and f:
                if not in_tub(f):
                    return "that filter is not in the tub"
                f["stints"][-1][1] = ts
                event(f, "removed", ts)
            elif action == "swap":
                out_f, in_f = find.get(body.get("outId")), find.get(body.get("inId"))
                if not out_f or not in_f or not in_tub(out_f) or in_tub(in_f):
                    return "can't swap those filters"
                out_f["stints"][-1][1] = ts
                event(out_f, "removed", ts)
                in_f["stints"].append([ts, 0])
                event(in_f, "installed", ts)
            elif action == "clean" and f:
                f["cleanedAt"], f["pumpSecSinceClean"] = ts, 0
                event(f, "cleaned", ts)
            elif action in ("rename", "delete", "install", "remove", "clean"):
                return "filter not found"
            else:
                return "unknown maintenance action"
            return None

    # ── water care (mirrors src/storage/water_log.cpp) ──

    def _seed_water(self, empty):
        now, day = int(time.time()), 86400
        cartridges = {"sanitizer": {"installedAt": 0, "pumpSec": 0, "lifeDays": 28},  # no dial until one is recorded
                      "mineral": {"installedAt": 0, "pumpSec": 0, "lifeDays": 120}}
        if empty:
            return {"system": "frog", "nextId": 1, "cartridges": cartridges, "entries": []}
        cartridges["sanitizer"].update(installedAt=now - 31 * day, pumpSec=190 * 3600, dial=3)
        cartridges["mineral"].update(installedAt=now - 50 * day, pumpSec=300 * 3600)
        entries = [
            {"type": "cartridge", "ts": now - 50 * day, "slot": "mineral"},
            {"type": "reading", "ts": now - 45 * day, "san": 1.0, "ph": 7.4, "ta": 100, "th": 180},
            {"type": "cartridge", "ts": now - 31 * day, "slot": "sanitizer", "dial": 3},
            {"type": "reading", "ts": now - 20 * day, "san": 0.5, "ph": 7.8, "ta": 120},
            {"type": "dose", "ts": now - 20 * day, "chem": "pH Down", "amount": 1, "unit": "tbsp"},
            {"type": "dose", "ts": now - 9 * day, "chem": "Shock (non-chlorine)", "amount": 3, "unit": "oz",
             "note": "after a party"},
            {"type": "reading", "ts": now - 2 * day, "san": 0.25, "ph": 7.0, "ta": 70},
        ]
        for i, entry in enumerate(entries, 1):
            entry["id"] = i
        return {"system": "frog", "nextId": len(entries) + 1, "cartridges": cartridges, "entries": entries}

    def water_json(self):
        with self.lock:
            cartridges = {k: {**c, "pumpSec": int(c["pumpSec"])} for k, c in self.water["cartridges"].items()}
            return {**self.water, "cartridges": cartridges, "now": int(time.time())}

    def water_action(self, body):
        with self.lock:
            w, now = self.water, int(time.time())
            ts = int(body.get("ts", now))
            if ts > now + 600:
                return "date must not be in the future"

            def add(entry):
                w["entries"] = (w["entries"] + [{"id": w["nextId"], "ts": ts, **entry}])[-150:]
                w["nextId"] += 1

            def note():
                text = str(body.get("note", "")).strip()
                return {"note": text} if text else {}

            action, slot = body.get("action"), body.get("slot")
            if action == "reading":
                pads = {k: float(body[k]) for k in ("san", "ph", "ta", "th") if body.get(k) is not None}
                if body.get("sanLevel"):
                    pads["sanLevel"] = int(body["sanLevel"])
                if not pads:
                    return "enter at least one reading"
                if not 6 <= pads.get("ph", 7) <= 9:
                    return "pH must be 6.0 to 9.0"
                add({"type": "reading", **pads, **note()})
            elif action == "dose":
                chem, amount, unit = str(body.get("chem", "")).strip(), body.get("amount"), body.get("unit", "")
                if not chem or len(chem) > 24:
                    return "pick a chemical"
                if not isinstance(amount, (int, float)) or amount <= 0:
                    return "amount must be more than 0"
                add({"type": "dose", "chem": chem, "amount": amount, "unit": unit, **note()})
            elif action == "replace" and slot in w["cartridges"]:
                c = w["cartridges"][slot]
                c.update(installedAt=ts, pumpSec=0)
                entry = {"type": "cartridge", "slot": slot}
                if slot == "sanitizer":
                    if body.get("dial") is not None:
                        c["dial"] = int(body["dial"])
                    if c.get("dial") is not None:
                        entry["dial"] = c["dial"]
                add(entry)
            elif action == "dial":
                if not 0 <= int(body.get("dial", -1)) <= 6:
                    return "dial must be 0 to 6"
                w["cartridges"]["sanitizer"]["dial"] = int(body["dial"])
                add({"type": "dial", "dial": int(body["dial"])})
            elif action == "life" and slot in w["cartridges"]:
                if not 1 <= int(body.get("days", 0)) <= 365:
                    return "replace every 1 to 365 days"
                w["cartridges"][slot]["lifeDays"] = int(body["days"])
            elif action == "system" and body.get("system") in ("frog", "chlorine", "bromine"):
                w["system"] = body["system"]
            elif action == "delete":
                before = len(w["entries"])
                w["entries"] = [e for e in w["entries"] if e["id"] != body.get("id")]
                if len(w["entries"]) == before:
                    return "entry not found"
            else:
                return "unknown water action"
            return None

    # ── simulation ──

    def _fetch_outside(self):
        """The same Open-Meteo history the browser charts, so simulated heater time can follow it."""
        location = self.config.get("location")
        if not location:
            return {}, {}
        params = urllib.parse.urlencode({
            "latitude": location["lat"], "longitude": location["lon"], "hourly": "temperature_2m",
            "daily": "temperature_2m_mean", "temperature_unit": "fahrenheit", "timezone": "auto",
            "past_days": 14, "forecast_days": 1})
        try:
            with urllib.request.urlopen(f"https://api.open-meteo.com/v1/forecast?{params}", timeout=6) as response:
                data = json.load(response)
        except Exception as error:  # offline is fine: fall back to made-up weather
            print(f"[mock] outside temperature unavailable ({error}); using made-up weather")
            return {}, {}
        offset = data.get("utc_offset_seconds", 0)
        daily = {int(d.replace("-", "")): t for d, t in zip(data["daily"]["time"], data["daily"]["temperature_2m_mean"])
                 if t is not None}
        # Times are the location's local time; read them as UTC, then remove the location's offset.
        hourly = {calendar.timegm(time.strptime(h, "%Y-%m-%dT%H:%M")) - offset: t
                  for h, t in zip(data["hourly"]["time"], data["hourly"]["temperature_2m"]) if t is not None}
        return daily, hourly

    def _outside_at(self, epoch):
        """Outside °F for an hour: the fetched value, or a made-up daily swing around 50°F."""
        if self.outside_hourly:
            nearest = min(self.outside_hourly, key=lambda t: abs(t - epoch))
            if abs(nearest - epoch) <= 3 * 3600:
                return self.outside_hourly[nearest]
        hour = datetime.fromtimestamp(epoch).hour
        return 50 - 10 * math.cos((hour - 4) / 24 * 2 * math.pi)

    def _fake_history(self):
        # Heater time rises as it gets colder outside (about +22 min per 10°F), plus day-to-day noise.
        days = []
        today = date.today()
        for back in range(13, 0, -1):
            day = today - timedelta(days=back)
            key = int(day.strftime("%Y%m%d"))
            outside = self.outside_daily.get(key, 50 + random.uniform(-12, 12))
            # Turned down to 96°F on the coldest days, like you might; heat loss follows water minus outside.
            set_temp = 96.0 if outside < 46 else 100.0
            water = round(set_temp - random.uniform(0.0, 1.0), 1)
            minutes = max(10, min(360, 2.2 * (water - outside) - 20 + random.gauss(0, 6)))
            weekday = day.weekday() < 5
            heating, low, high = int(minutes * 60), random.randint(300, 600) * 60, random.randint(0, 45) * 60
            days.append({
                "date": key,
                "heatingSec": heating,
                "pumpLowSec": low,
                "pumpHighSec": high,
                "filterSec": 6 * 3600,
                "pump2Sec": 0,
                "countedSec": 86400,
                # On weekdays a slice of the running lands in the 5-9 PM peak.
                "heatingPeakSec": int(heating * random.uniform(0.05, 0.2)) if weekday else 0,
                "pumpLowPeakSec": int(low * random.uniform(0.05, 0.2)) if weekday else 0,
                "pumpHighPeakSec": int(high * random.uniform(0.3, 0.8)) if weekday else 0,
                "pump2PeakSec": 0,
                "countedPeakSec": 4 * 3600 if weekday else 0,
                "waterTemp": water,
                "setTemp": set_temp,
            })
        days.append(self._empty_day(today))
        return days

    @staticmethod
    def _empty_day(day):
        return {"date": int(day.strftime("%Y%m%d")), **{key: 0 for key in (
            "heatingSec", "pumpLowSec", "pumpHighSec", "filterSec", "pump2Sec", "countedSec", "heatingPeakSec",
            "pumpLowPeakSec", "pumpHighPeakSec", "pump2PeakSec", "countedPeakSec")}}

    def _in_peak(self):
        """Weekday peak hours from the config; holidays aren't simulated."""
        now, tou = datetime.now(), self.config["tou"]
        return now.weekday() < 5 and tou["peakStart"] <= now.hour < tou["peakEnd"]

    def _fake_hours(self):
        # The water loses heat faster when it's colder outside; the heater tops it back up to the set point.
        # On cold nights the spa is turned down to 96°F from midnight to 6 AM.
        hours, temp = [], 99.6
        top = int(time.time()) // 3600 * 3600
        for back in range(47, 0, -1):
            start = top - back * 3600
            outside = self._outside_at(start)
            set_temp = 96.0 if datetime.fromtimestamp(start).hour < 6 and outside < 42 else 100.0
            temp -= 0.012 * (temp - outside) + random.uniform(-0.03, 0.03)
            heating = 0
            if temp < set_temp - 1:
                minutes = min(60, (set_temp - temp) / 1.6 * 60)
                heating = int(minutes * 60)
                temp += 1.6 * minutes / 60
            hours.append({"start": start, "heatingSec": heating, "waterTemp": round(temp, 1), "setTemp": set_temp})
        hours.append({"start": top, "heatingSec": 0, "waterTemp": round(temp, 1), "setTemp": self.state["setTemp"]})
        return hours

    def _filter_running(self, now):
        minutes = now.hour * 60 + now.minute
        for cycle in (1, 2):
            if cycle == 2 and not self.filters["cycle2Enabled"]:
                continue
            start = self.filters[f"cycle{cycle}Start"] * 60 + self.filters[f"cycle{cycle}StartMinute"]
            length = self.filters[f"cycle{cycle}Duration"] * 60 + self.filters[f"cycle{cycle}DurationMinute"]
            if (minutes - start) % (24 * 60) < length:
                return cycle
        return 0

    def tick(self):
        with self.lock:
            now_m = time.monotonic()
            dt = now_m - self.last_tick
            self.last_tick = now_m
            for item in [p for p in self.pending if p[0] <= now_m]:
                self.pending.remove(item)
                item[1]()

            s = self.state
            s["activeFilterCycle"] = self._filter_running(datetime.now())
            if s["hold"]:
                remaining = self.hold_until - now_m
                s["holdMinutes"] = max(0, math.ceil(remaining / 60))
                if remaining <= 0:
                    s["hold"] = False
                    s["holdMinutes"] = 0
            if s["hold"]:
                s["heating"] = s["heatPending"] = False
                s["jet1Speed"] = s["jet2Speed"] = 0
            else:
                if s["waterTemp"] < s["setTemp"] - 1:
                    s["heating"] = True
                elif s["waterTemp"] >= s["setTemp"]:
                    s["heating"] = False
                if s["heating"]:
                    s["waterTemp"] = min(s["setTemp"], s["waterTemp"] + self.heat_rate * dt)
                else:
                    s["waterTemp"] = max(60.0, s["waterTemp"] - self.heat_rate * 0.1 * dt)
                if self.heat_holds_pump and (s["heating"] or s["activeFilterCycle"]) and s["jet1Speed"] == 0:
                    s["jet1Speed"] = 1  # the spa runs pump 1 low to heat and filter

            today = self.runtime[-1]
            if today["date"] != int(date.today().strftime("%Y%m%d")):
                self.runtime.append(self._empty_day(date.today()))
                self.runtime = self.runtime[-30:]
                today = self.runtime[-1]
            if s["jet1Speed"] > 0:
                for f in self.maint["filters"]:
                    if f["stints"] and f["stints"][-1][1] == 0:
                        f["pumpSec"] += dt
                        f["pumpSecSinceClean"] += dt
                if self.water["system"] == "frog":
                    for c in self.water["cartridges"].values():
                        if c["installedAt"]:
                            c["pumpSec"] += dt
            top = int(time.time()) // 3600 * 3600
            if self.hours[-1]["start"] != top:
                self.hours = (self.hours + [{"start": top, "heatingSec": 0, "waterTemp": s["waterTemp"]}])[-48:]
            hour = self.hours[-1]
            hour["heatingSec"] += dt if s["heating"] else 0
            if not self.water_unknown:  # history skips readings the spa didn't report
                hour["waterTemp"] = round(s["waterTemp"], 1)
                today["waterTemp"] = round(s["waterTemp"], 1)
            hour["setTemp"] = s["setTemp"]
            today["setTemp"] = s["setTemp"]
            peak = self._in_peak()
            for key, running in (("heating", s["heating"]), ("pumpLow", s["jet1Speed"] == 1),
                                 ("pumpHigh", s["jet1Speed"] == 2), ("pump2", s["jet2Speed"] > 0), ("counted", True)):
                if running:
                    today[f"{key}Sec"] += dt
                    today[f"{key}PeakSec"] += dt if peak else 0
            today["filterSec"] += dt if s["activeFilterCycle"] else 0

            self._step_pump_command(now_m)
            self._step_smart()
            if self.capture_active:
                self.capture_lines.append(
                    f"{int(now_m * 1000)} status water={s['waterTemp']:.0f} set={s['setTemp']:.0f} "
                    f"heating={int(s['heating'])} hold={int(s['hold'])} jetSpeed={s['jet1Speed']}/{s['jet2Speed']} "
                    f"light={int(s['light'])}")
                self.capture_lines = self.capture_lines[-500:]

    def _pump_speeds(self, pump):
        if pump == 2 and self.config["capabilities"].get("hideJet2"):
            return 0
        return self.pump_speeds[pump - 1]

    def _press_pump(self, pump):
        """One press of a jet button, as the spa would handle it."""
        key = f"jet{pump}Speed"
        speeds = self.pump_speeds[pump - 1]
        nxt = (self.state[key] + 1) % (speeds + 1)
        held = self.heat_holds_pump and pump == 1 and (self.state["heating"] or self.state["activeFilterCycle"])
        if held and nxt == 0:
            nxt = 1  # can't turn pump 1 off while it's needed for heating or filtering
        self.state[key] = nxt

    def _step_pump_command(self, now_m):
        # Mirrors SpaController::processPumpCommand: press, wait for status, repeat.
        cmd = self.pump_cmd_internal
        if not cmd or now_m < cmd["next_check"]:
            return
        current = self.state[f"jet{cmd['pump']}Speed"]

        def finish(ok, message):
            self.pump_cmd.update(active=False, ok=ok, message=message, seq=self.pump_cmd["seq"] + 1)
            self.pump_cmd_internal = None

        if current == cmd["target"]:
            return finish(True, f"Jet {cmd['pump']} set to {SPEED_NAMES[cmd['target']]}")
        if cmd["presses"] > 0:
            if current == cmd["start"] and current != cmd["last"]:
                return finish(False, f"The spa won't set Jet {cmd['pump']} to {SPEED_NAMES[cmd['target']]} "
                                     "right now (it runs pump 1 for heating and filtering)")
            cmd["last"] = current
        if cmd["presses"] >= 4:
            return finish(False, f"Jet {cmd['pump']} stopped at {SPEED_NAMES[current]}")
        cmd["presses"] += 1
        pump = cmd["pump"]
        self.pending.append((now_m + PRESS_LATENCY_S, lambda: self._press_pump(pump)))
        cmd["next_check"] = now_m + PRESS_LATENCY_S + 0.9

    # ── commands (same actions and messages as SpaController::handleCommand) ──

    def command(self, cmd):
        with self.lock:
            return self._command(cmd)

    def _command(self, cmd):
        s = self.state
        action = cmd.get("action", "")
        later = lambda delay, fn: self.pending.append((time.monotonic() + delay, fn))

        if action == "setTemp":
            low, high = (80, 104) if s["tempRangeHigh"] else (50, 99)
            value = cmd.get("value", -1)
            if not isinstance(value, (int, float)) or not low <= value <= high:
                return 400, f"set temperature must be {low}-{high} F"
            later(0.6, lambda: s.update(setTemp=float(value)))
            self._manual_change(float(value))
            return 202, "command accepted"
        if action in ("soak", "cancelSoak", "resumeSmart"):
            return self._smart_command(action, cmd)
        if action in ("setFilter1", "setFilter2"):
            n = 2 if action == "setFilter2" else 1
            enabled = n == 1 or bool(cmd.get("enabled"))
            update = {f"cycle{n}Start": cmd.get("startHour", 0) if enabled else 0,
                      f"cycle{n}StartMinute": cmd.get("startMinute", 0),
                      f"cycle{n}Duration": cmd.get("durationHour", 0) if enabled else 0,
                      f"cycle{n}DurationMinute": cmd.get("durationMinute", 0)}
            if n == 2:
                update["cycle2Enabled"] = enabled

            def apply():
                self.filters.update(update)
                self.filter_seq += 1
            later(1.4, apply)
            return 202, "command accepted"
        if action == "readFilterCycles":
            later(0.4, lambda: setattr(self, "filter_seq", self.filter_seq + 1))
            return 202, "command accepted"
        if action == "setPump":
            pump, value = cmd.get("pump", 0), cmd.get("value", -1)
            if pump not in (1, 2) or self._pump_speeds(pump) == 0:
                return 400, "this spa does not have that pump"
            if not 0 <= value <= self.pump_speeds[pump - 1]:
                return 400, "pump speed must be 0-2"
            if self.pump_cmd_internal:
                return 400, "a pump change is already in progress"
            if s[f"jet{pump}Speed"] == value:
                return 202, "pump already at that speed"
            start = s[f"jet{pump}Speed"]
            self.pump_cmd_internal = {"pump": pump, "target": value, "start": start, "last": start,
                                      "presses": 0, "next_check": 0}
            self.pump_cmd.update(active=True, pump=pump, target=value)
            return 202, "pump change started"
        if action == "togglePump":
            pump = cmd.get("pump", 0)
            if pump not in (1, 2) or self._pump_speeds(pump) == 0:
                return 400, "this spa does not have that pump"
            if self.pump_cmd_internal:
                return 400, "a pump change is already in progress"
            later(PRESS_LATENCY_S, lambda: self._press_pump(pump))
            return 202, "command accepted"
        if action == "light":
            if "value" in cmd and cmd["value"] == s["light"]:
                return 202, "light already on" if s["light"] else "light already off"
            later(0.6, lambda: s.update(light=not s["light"]))
            return 202, "command accepted"
        if action == "hold":
            if "value" in cmd and cmd["value"] == s["hold"]:
                return 202, "already in hold" if s["hold"] else "not in hold"

            def toggle_hold():
                s["hold"] = not s["hold"]
                self.hold_until = time.monotonic() + 3600
            later(0.6, toggle_hold)
            return 202, "command accepted"
        if action == "tempRange":
            if cmd.get("value") not in ("high", "low"):
                return 400, "range must be high or low"
            high = cmd["value"] == "high"
            if high == s["tempRangeHigh"]:
                return 202, "already in high range" if high else "already in low range"

            def toggle_range():  # like the pack, each range keeps its own set temperature
                self.range_set_temp[s["tempRangeHigh"]] = s["setTemp"]
                s["tempRangeHigh"] = high
                s["setTemp"] = self.range_set_temp[high]
            later(0.6, toggle_range)
            return 202, "command accepted"
        if action == "clearReminder":
            later(0.6, lambda: s.update(reminder=0))
            return 202, "command accepted"
        if action == "readFaultLog":
            self.fault_reading_until = time.monotonic() + 1.5
            later(1.5, lambda: setattr(self, "fault_seq", self.fault_seq + 1))
            return 202, "reading fault log"
        if action == "refreshSpaInfo":
            later(0.8, lambda: setattr(self, "spa_info_seq", self.spa_info_seq + 1))
            return 202, "spa info requested"
        if action == "syncTime":
            return 202, "command accepted"
        if action == "startStatusCapture":
            self.capture_active, self.capture_lines = True, ["capture started"]
            return 202, "status capture started"
        if action == "stopStatusCapture":
            self.capture_active = False
            self.capture_lines.append("capture stopped")
            return 202, "status capture stopped"
        if action == "clearStatusCapture":
            self.capture_lines = []
            return 202, "status capture cleared"
        return 400, "unknown command action"

    # ── JSON views ──

    def state_json(self):
        with self.lock:
            s = dict(self.state)
            speeds = self.pump_speeds
            now = datetime.now()
            data = {
                **s,
                "waterTemp": None if self.water_unknown else round(s["waterTemp"]),
                "lastWaterTemp": self.last_water if self.water_unknown else round(s["waterTemp"]),
                "lastWaterTempAt": self.last_water_at if self.water_unknown else int(time.time()),
                "jet1": s["jet1Speed"] != 0,
                "jet2": s["jet2Speed"] != 0,
                "setTempRange": {"min": 80, "max": 104} if s["tempRangeHigh"] else {"min": 50, "max": 99},
                "filterCycles": {**self.filters, "valid": True, "seq": self.filter_seq},
                "pumpCommand": dict(self.pump_cmd),
                "spaInfoSeq": self.spa_info_seq,
                "faultLogSeq": self.fault_seq,
                "faultLogReading": time.monotonic() < self.fault_reading_until,
                "spaTime": now.strftime("%H:%M"),
                "valid": True,
                "statusCaptureActive": self.capture_active,
                "statusCaptureLines": len(self.capture_lines),
                "smart": self.smart_json(int(time.time())),
                "capabilities": {
                    "detected": True, "jet1": speeds[0] > 0, "jet2": self._pump_speeds(2) > 0,
                    "pump1Speeds": speeds[0], "pump2Speeds": self._pump_speeds(2), "light": True,
                    "pump2Installed": speeds[1] > 0, **self.config["capabilities"],
                },
            }
            return {"type": "state", "data": data}

    def spa_info_json(self):
        with self.lock:
            return {
                "equipment": {"pumps": self.pump_speeds[:6] + [0] * (6 - len(self.pump_speeds[:6])),
                              "light1": True, "light2": False, "blower": False, "circulationPump": False,
                              "aux1": False, "aux2": False, "mister": False},
                "system": {"model": "MOCK-SPA", "softwareVersion": "M100_225 V43.0", "setupNumber": 1,
                           "heaterVoltage": 240, "dipSwitches": "1000000000000000"},
                "tempRanges": {"lowMin": 50, "lowMax": 99, "highMin": 80, "highMax": 104},
                "seq": self.spa_info_seq,
            }

    def runtime_json(self):
        with self.lock:
            whole = lambda item: {k: v if k in ("waterTemp", "setTemp") else round(v) for k, v in item.items()}
            return {"days": [whole(day) for day in self.runtime], "hours": [whole(hour) for hour in self.hours]}

    def thermal_json(self):
        """Mirrors /api/thermal: a few measured heats (still an estimate) and enough cool-downs to be learned."""
        with self.lock:
            now = int(time.time())
            power = self.config["power"]
            seed = power["heaterKw"] * 3412.14 / (power["tubGallons"] * 8.34)
            heat = [{"t": now - d * 86400 - 3600, "v": v, "a": 96, "b": 102, "o": 41 + d, "m": 62}
                    for d, v in ((1, 6.1), (3, 5.8))]
            cool = [{"t": now - d * 43200, "v": v, "a": 102, "b": 98, "o": 38 + d, "m": 400}
                    for d, v in enumerate((0.011, 0.013, 0.012, 0.014, 0.010, 0.012), start=1)]
            heater = (seed + sum(r["v"] for r in heat)) / (len(heat) + 1)
            loss = sorted(r["v"] for r in cool)[len(cool) // 2 - 1: len(cool) // 2 + 1]
            top = now - now % 3600
            hours = [{"t": t, "temp": round(self._outside_at(t), 1)} for t in range(top - 6 * 3600, top + 36 * 3600, 3600)]
            return {
                "model": {"heaterRate": heater, "heaterRateSeed": seed, "lossCoeff": sum(loss) / 2, "lossCoeffSeed": 0.012},
                "runs": {"heat": heat, "cool": cool},
                "weather": {"hasLocation": bool(self.config.get("location")), "fetchedAt": now - 900,
                            "now": round(self._outside_at(now), 1), "hours": hours},
            }

    # ── smart heating (mirrors src/scheduler/heat_controller.cpp and heat_planner.cpp, simplified) ──

    def _price(self, epoch):
        when, tou = datetime.fromtimestamp(epoch), self.config["tou"]
        rates = tou["rates"]
        peak = when.weekday() < 5 and tou["peakStart"] <= when.hour < tou["peakEnd"]
        a, b = rates["summerStartMonth"], rates["summerEndMonth"]
        summer = a <= when.month <= b if a <= b else (when.month >= a or when.month <= b)
        on, off = (rates["summerPeak"], rates["summerOffPeak"]) if summer else (rates["winterPeak"], rates["winterOffPeak"])
        if not on and not off:
            return 2.0 if peak else 1.0
        return on if peak else off

    @staticmethod
    def _simulate(temp, heat, outside, top):
        hours = PLAN_STEP / 3600
        coast = outside + (temp - outside) * math.exp(-PLAN_LOSS * hours)
        if not heat or coast >= top:
            return coast, 0.0
        level = outside + PLAN_HEATER_RATE / PLAN_LOSS
        full = level + (temp - level) * math.exp(-PLAN_LOSS * hours)
        if full <= top:
            return full, 1.0
        return top, (top - coast) / (full - coast)

    def _earliest(self, water, start, target):
        temp = water
        for i in range(144):
            if temp >= target - 0.25:
                return start + i * PLAN_STEP
            temp = self._simulate(temp, True, self._outside_at(start + i * PLAN_STEP), target)[0]
        return 0

    @staticmethod
    def _likelihood(weekend, hour):
        """The starting likelihoods from src/scheduler/usage_model.cpp."""
        return (0.02 if hour < 7 else 0.03 if hour < 10 else (0.15 if weekend else 0.05) if hour < 17
                else 0.15 if hour < 18 else 0.4 if hour < 23 else 0.1)

    def _ready_within(self, epoch):
        """Hours of heat-up allowed at a time."""
        when = datetime.fromtimestamp(epoch)
        p = self._likelihood(when.weekday() >= 5, when.hour)
        return 0.5 if p >= 0.25 else 2.0 if p >= 0.04 else 4.0

    def _readiness(self, comfort, hours, outside):
        level = outside + PLAN_HEATER_RATE / PLAN_LOSS
        return comfort if level <= comfort else level - (level - comfort) * math.exp(PLAN_LOSS * hours)

    def _plan(self, water, start, count, target, ready, hours, auto=False):
        floor = self.config["smart"]["minTemp"]
        steps = []
        for i in range(count):
            end = start + (i + 1) * PLAN_STEP
            outside = self._outside_at(end - 450)
            need = floor if floor < target else None
            if auto:
                ready_temp = min(self.comfort, self._readiness(self.comfort, self._ready_within(end), outside))
                need = max(need or floor, ready_temp)
            if ready is not None and ready <= end < ready + hours * 3600:
                need = target
            steps.append({"price": self._price(start + i * PLAN_STEP), "outside": outside, "min": need})
        lo = coast = water
        for step in steps:
            coast = self._simulate(coast, False, step["outside"], target)[0]
            lo = min(lo, coast)
        lo -= 0.25
        points = int(math.ceil((max(water, target) + 0.25 - lo) / 0.25)) + 1
        short = lambda step, temp: 0 if step["min"] is None else max(0, step["min"] - temp)

        def cost_at(row, temp):
            pos = min(max((temp - lo) / 0.25, 0), points - 1)
            i = min(int(pos), points - 2)
            return row[i] + (row[i + 1] - row[i]) * (pos - i)
        kwh = self.config["power"]["heaterKw"] * PLAN_STEP / 3600
        later, policy = [0.0] * points, [None] * count
        for s in range(count - 1, -1, -1):
            step, row, choice = steps[s], [0.0] * points, [False] * points
            for i in range(points):
                temp = lo + i * 0.25
                off = self._simulate(temp, False, step["outside"], target)
                on = self._simulate(temp, True, step["outside"], target)
                off_cost = short(step, off[0]) * 100 + cost_at(later, off[0])
                on_cost = on[1] * kwh * step["price"] + short(step, on[0]) * 100 + cost_at(later, on[0])
                choice[i] = on_cost < off_cost - 1e-6
                row[i] = on_cost if choice[i] else off_cost
            policy[s], later = choice, row
        temp, out, cost, worst, used = water, [], 0.0, 0.0, 0.0
        for s, step in enumerate(steps):
            temp, f = self._simulate(temp, policy[s][int(min(max((temp - lo) / 0.25, 0), points - 1))],
                                     step["outside"], target)
            cost, used, worst = cost + f * kwh * step["price"], used + f * kwh, max(worst, short(step, temp))
            out.append({"heat": round(f, 2), "temp": round(temp, 1), "price": step["price"],
                        "outside": round(step["outside"]), **({"min": step["min"]} if step["min"] else {})})
        return {"steps": out, "cost": cost, "kwh": used, "shortfall": worst}

    def plan_json(self, query):
        with self.lock:
            now = int(time.time())
            start = now - now % PLAN_STEP
            water = self.state["waterTemp"]
            target = float(query.get("target", [self.state["setTemp"]])[0])
            hours = float(query.get("hours", [2])[0])
            requested = int(query.get("readyAt", [0])[0])
            asap = self._earliest(water, start, target)
            ready = max(requested, asap) if requested else asap
            count = min(144, (ready + int(hours * 3600) - start) // PLAN_STEP + 2)
            plan = self._plan(water, start, count, target, ready, hours)
            leave = self._plan(water, start, count, target, asap, (ready - asap) / 3600 + hours)
            return {"start": start, "stepSec": PLAN_STEP, "waterTemp": water, "target": target, "hours": hours,
                    "readyAt": ready, "asapReadyAt": asap, "reachable": asap != 0,
                    "kwh": round(plan["kwh"], 2), "cost": round(plan["cost"], 3), "shortfall": round(plan["shortfall"], 1),
                    "leaveOn": {"kwh": round(leave["kwh"], 2), "cost": round(leave["cost"], 3)}, "steps": plan["steps"]}

    def _in_use(self):
        s = self.state
        return s["jet1Speed"] == 2 or s["jet2Speed"] > 0 or s["blower"] or s["light"]

    @property
    def comfort(self):
        return float(self.config["smart"]["readyTemp"])

    def _manual_change(self, value):
        if self.soak or self.config["smart"]["auto"]:
            self.soak = None
            self.auto_plan = None
            now = int(time.time())
            self.manual = {"since": now, "temp": value, "until": now + 4 * 3600}

    def _smart_command(self, action, cmd):
        s, now = self.state, int(time.time())
        if action == "resumeSmart":
            self.manual = None
            return 202, "smart heating resumed"
        if action == "cancelSoak":
            if self.soak:
                if self._in_use():
                    self.manual = {"since": now, "temp": s["setTemp"], "until": now + 3600}
                elif not self.config["smart"]["auto"]:
                    s["setTemp"] = self.soak["restore"]
                self.soak = None
            return 202, "soak cancelled"
        target, hours, ready = float(cmd.get("target", s["setTemp"])), float(cmd.get("hours", 2)), int(cmd.get("readyAt") or 0)
        if not 80 <= target <= 104:
            return 400, "target must be 80-104°F"
        if not 0.5 <= hours <= 6:
            return 400, "soak length must be 30 minutes to 6 hours"
        if ready and not now <= ready <= now + 30 * 3600:
            return 400, "ready time must be within the next 30 hours"
        start = now - now % PLAN_STEP
        asap = not ready
        ready = ready or self._earliest(s["waterTemp"], start, target)
        count = min(144, (ready + int(hours * 3600) - start) // PLAN_STEP + 2)
        restore = self.soak["restore"] if self.soak else s["setTemp"]
        self.soak = {"target": round(target), "readyAt": ready, "hours": hours, "asap": asap, "restore": restore,
                     "start": start, "plan": None if asap else self._plan(s["waterTemp"], start, count, target, ready, hours)}
        self.manual = None
        return 202, "soak scheduled"

    def _soak_setpoint(self, now):
        soak = self.soak
        if soak["asap"] or now >= soak["readyAt"] or not soak["plan"]:
            return soak["target"]
        return self._plan_setpoint(soak["plan"]["steps"], soak["start"], soak["target"], now)

    def _plan_setpoint(self, steps, start, target, now):
        s = (now - start) // PLAN_STEP
        if s >= len(steps):
            return target
        run = []
        heating = steps[s]["heat"] > 0
        while s < len(steps) and (steps[s]["heat"] > 0) == heating:
            run.append(steps[s]["temp"])
            s += 1
        if heating:
            return min(target, math.ceil(max(run) - 0.05))
        return max(self.config["smart"]["minTemp"], math.floor(min(run)) - 2)

    def _step_smart(self):
        now = int(time.time())
        if not self.state["tempRangeHigh"]:
            self.soak, self.auto_plan = None, None  # like the firmware: the low range turns smart heating off
            return
        if self.manual:
            if self._in_use():
                self.manual["until"] = max(self.manual["until"], now + 3600)
            elif now >= self.manual["until"]:
                self.manual = None
            return
        if not self.soak:
            if self.config["smart"]["auto"]:
                start = now - now % PLAN_STEP
                if not self.auto_plan or self.auto_plan["start"] != start:
                    plan = self._plan(self.state["waterTemp"], start, 144, self.comfort, None, 0, auto=True)
                    hold = self._plan(self.state["waterTemp"], start, 144, self.comfort, start, 36)
                    # Both sides end the window at comfort, like the firmware's comparison.
                    fair = self._plan(self.state["waterTemp"], start, 144, self.comfort, start + 143 * PLAN_STEP, 0.25, auto=True)
                    self.auto_plan = {"start": start, "steps": plan["steps"], "cost": plan["cost"], "kwh": plan["kwh"],
                                      "compare": {"holdCost": round(hold["cost"], 3), "holdKwh": round(hold["kwh"], 2),
                                                  "smartCost": round(fair["cost"], 3), "smartKwh": round(fair["kwh"], 2)}}
                want = float(self._plan_setpoint(self.auto_plan["steps"], start, self.comfort, now))
                if not (self._in_use() and want < self.state["setTemp"]):
                    self.state["setTemp"] = want
            else:
                self.auto_plan = None
            return
        soak = self.soak
        if self._in_use() and now >= soak["readyAt"]:
            soak["lastUse"] = now
        end = max(soak["readyAt"] + int(soak["hours"] * 3600), soak.get("lastUse", 0) + 1800)
        if now >= end and not self._in_use():
            if not self.config["smart"]["auto"]:
                self.state["setTemp"] = soak["restore"]
            self.soak = None
            return
        want = float(self._soak_setpoint(now))
        if not (self._in_use() and want < self.state["setTemp"]):
            self.state["setTemp"] = want

    def smart_json(self, now, plan=False):
        auto = self.config["smart"]["auto"]
        mode = ("lowRange" if not self.state["tempRangeHigh"] else "manual" if self.manual else "soak" if self.soak
                else "auto" if auto else "idle")
        out = {"mode": mode,
               "comfortTemp": self.comfort}
        water, outside = self.state["waterTemp"], self._outside_at(now)
        level = outside + PLAN_HEATER_RATE / PLAN_LOSS
        out["readyInMin"] = (0 if water >= self.comfort else -1 if self.comfort >= level else
                             round(math.log((level - water) / (level - self.comfort)) / PLAN_LOSS * 60))
        if out["mode"] == "auto" and self.auto_plan:
            steps, s = self.auto_plan["steps"], (now - self.auto_plan["start"]) // PLAN_STEP
            out["auto"] = {"phase": "heating" if s < len(steps) and steps[s]["heat"] > 0 else "waiting",
                           "nextHeatAt": next((max(now, self.auto_plan["start"] + i * PLAN_STEP)
                                               for i in range(s, len(steps)) if steps[i]["heat"] > 0), None)}
        if self.manual:
            out["manual"] = dict(self.manual)
        if self.soak:
            soak = self.soak
            info = {k: soak[k] for k in ("target", "readyAt", "hours", "asap")}
            info["endsAt"] = max(soak["readyAt"] + int(soak["hours"] * 3600), soak.get("lastUse", 0) + 1800)
            if now >= soak["readyAt"]:
                info["phase"] = "ready"
            elif soak["asap"]:
                info["phase"] = "heating"
            else:
                steps, s = soak["plan"]["steps"], (now - soak["start"]) // PLAN_STEP
                info["phase"] = "heating" if s < len(steps) and steps[s]["heat"] > 0 else "waiting"
                info["nextHeatAt"] = next((max(now, soak["start"] + i * PLAN_STEP) for i in range(s, len(steps))
                                           if steps[i]["heat"] > 0), None)
                info["cost"] = round(soak["plan"]["cost"], 2)
            out["soak"] = info
        if plan and not self.soak and self.auto_plan and not self.manual:
            p = self.auto_plan
            out["plan"] = {"start": p["start"], "stepSec": PLAN_STEP, "target": self.comfort, "kwh": round(p["kwh"], 2),
                           "cost": round(p["cost"], 3), "compare": p["compare"], "steps": p["steps"]}
        elif plan:
            p = self.soak and self.soak["plan"]
            out["plan"] = {"start": self.soak["start"], "stepSec": PLAN_STEP, "kwh": round(p["kwh"], 2),
                           "cost": round(p["cost"], 3), "steps": p["steps"]} if p else None
        if plan:
            out["usage"] = {kind: [self._likelihood(kind == "weekend", slot // 2) for slot in range(48)]
                            for kind in ("workday", "weekend")}
        return out

    def config_json(self):
        with self.lock:
            return {"type": "config", "data": {**self.config, "filterCycles": dict(self.filters)}}

    def save_config(self, body):
        with self.lock:
            for key in ("timezone", "authEnabled"):
                if key in body:
                    self.config[key] = body[key]
            if isinstance(body.get("location"), dict):
                loc = body["location"]
                self.config["location"] = {"lat": float(loc["lat"]), "lon": float(loc["lon"])} if loc else None
            if body.get("authPassword"):
                self.config["authPasswordSet"] = True
            self.config["capabilities"].update(body.get("capabilities", {}))
            tou = body.get("tou", {})
            self.config["tou"]["rates"].update(tou.pop("rates", {}))
            self.config["tou"].update(tou)
            self.config["power"].update(body.get("power", {}))
            was_auto = self.config["smart"]["auto"]
            self.config["smart"].update(body.get("smart", {}))
            if body.get("smart"):
                self.auto_plan = None  # replan with the new settings, as the firmware does
            if was_auto and not self.config["smart"]["auto"] and not self.soak and not self.manual:
                self.state["setTemp"] = self.comfort  # like the firmware: turning Auto off puts comfort back

    def force(self, fields):
        """POST /mock: overwrite any state field, plus a few extras."""
        with self.lock:
            if "pumps" in fields:
                self.pump_speeds = [int(x) for x in str(fields.pop("pumps")).split(",")] + [0, 0]
                self.spa_info_seq += 1
            if fields.get("hold"):
                self.hold_until = time.monotonic() + 60 * fields.get("holdMinutes", 60)
            if fields.pop("faultLogEmpty", False):
                self.faults = []
            if "waterTemp" in fields:  # null: the spa stops reporting it; a number: reported again
                value = fields.pop("waterTemp")
                if value is None and not self.water_unknown:
                    self.last_water, self.last_water_at = round(self.state["waterTemp"]), int(time.time())
                self.water_unknown = value is None
                if value is not None:
                    self.state["waterTemp"] = float(value)
            if "lastWaterTempAgeMin" in fields:  # backdate the last reading to try the age display
                self.last_water_at = int(time.time()) - 60 * int(fields.pop("lastWaterTempAgeMin"))
            self.state.update({k: v for k, v in fields.items() if k in self.state})


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # browsers refuse a WebSocket upgrade over HTTP/1.0
    spa: MockSpa = None
    clients = []
    clients_lock = threading.Lock()

    def log_message(self, fmt, *args):
        if not self.path.startswith(("/api/state", "/api/status-capture")):
            print(f"[mock] {self.command} {self.path} {args[1] if len(args) > 1 else ''}")

    def _json(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            return json.loads(raw or b"{}")
        except json.JSONDecodeError:
            return None

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/ws" and self.headers.get("Upgrade", "").lower() == "websocket":
            return self._websocket()
        routes = {
            "/api/state": lambda: self.spa.state_json(),
            "/api/config": lambda: self.spa.config_json(),
            "/api/spa-info": lambda: self.spa.spa_info_json(),
            "/api/faults": lambda: {"reading": False, "seq": self.spa.fault_seq,
                                    "total": len(self.spa.faults), "entries": self.spa.faults},
            "/api/runtime": lambda: self.spa.runtime_json(),
            "/api/thermal": lambda: self.spa.thermal_json(),
            "/api/plan": lambda: self.spa.plan_json(urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)),
            "/api/smart": lambda: self.spa.smart_json(int(time.time()), plan=True),
            "/api/maintenance": lambda: self.spa.maintenance_json(),
            "/api/water": lambda: self.spa.water_json(),
            "/api/debug-log": None,
            "/api/status-capture": None,
        }
        if path in ("/api/debug-log", "/api/status-capture"):
            text = "\n".join(self.spa.capture_lines if path == "/api/status-capture" else []).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(text)))
            self.end_headers()
            return self.wfile.write(text)
        if path in routes:
            return self._json(200, routes[path]())
        if path.startswith("/api/"):
            return self._json(404, {"status": "not_found"})
        self._static(path)

    def do_DELETE(self):
        if self.path.startswith("/api/status-capture"):
            self.spa.capture_lines = []
        self.send_response(204)
        self.end_headers()

    def do_POST(self):
        body = self._body()
        if body is None:
            return self._json(400, {"status": "invalid_value", "message": "invalid json"})
        if self.path == "/api/cmd":
            code, message = self.spa.command(body)
            status = "accepted" if code == 202 else "invalid_value"
            return self._json(code, {"type": "cmd_result", "status": status, "message": message})
        if self.path == "/api/maintenance":
            error = self.spa.maintenance_action(body)
            if error:
                return self._json(400, {"status": "invalid_value", "message": error})
            return self._json(200, self.spa.maintenance_json())
        if self.path == "/api/water":
            error = self.spa.water_action(body)
            if error:
                return self._json(400, {"status": "invalid_value", "message": error})
            return self._json(200, self.spa.water_json())
        if self.path == "/api/config":
            self.spa.save_config(body)
            return self._json(200, self.spa.config_json())
        if self.path == "/mock":
            self.spa.force(body)
            return self._json(200, self.spa.state_json())
        self._json(404, {"status": "not_found"})

    def _static(self, path):
        file = (DATA_DIR / path.lstrip("/")).resolve()
        if path == "/" or not file.is_file() or DATA_DIR not in file.parents:
            file = DATA_DIR / "index.html"  # same fallback as the firmware
        body = file.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", mimetypes.guess_type(file.name)[0] or "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")  # edits show up on refresh
        self.end_headers()
        self.wfile.write(body)

    def _websocket(self):
        accept = base64.b64encode(hashlib.sha1((self.headers["Sec-WebSocket-Key"] + WS_GUID).encode()).digest())
        self.send_response(101)
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept.decode())
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True
        self.ws_lock = threading.Lock()
        with self.clients_lock:
            self.clients.append(self)
        try:
            send_ws(self, json.dumps(self.spa.state_json()))
            while True:  # read until the browser closes; text frames are commands, like the firmware
                header = self.rfile.read(2)
                if len(header) < 2:
                    break
                opcode, length = header[0] & 0x0F, header[1] & 0x7F
                if length == 126:
                    length = struct.unpack(">H", self.rfile.read(2))[0]
                elif length == 127:
                    length = struct.unpack(">Q", self.rfile.read(8))[0]
                mask = self.rfile.read(4) if header[1] & 0x80 else b"\0\0\0\0"
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(self.rfile.read(length)))
                if opcode == 0x8:
                    break
                if opcode == 0x1:
                    try:
                        code, message = self.spa.command(json.loads(payload))
                    except json.JSONDecodeError:
                        code, message = 400, "invalid json"
                    status = "accepted" if code == 202 else "invalid_value"
                    send_ws(self, json.dumps({"type": "cmd_result", "status": status, "message": message}))
        except (ConnectionError, OSError):
            pass
        finally:
            with self.clients_lock:
                if self in self.clients:
                    self.clients.remove(self)


def send_ws(client, text):
    data = text.encode()
    if len(data) < 126:
        header = struct.pack("!BB", 0x81, len(data))
    elif len(data) < 65536:
        header = struct.pack("!BBH", 0x81, 126, len(data))
    else:
        header = struct.pack("!BBQ", 0x81, 127, len(data))
    with client.ws_lock:
        client.wfile.write(header + data)
        client.wfile.flush()


def run_simulation(spa):
    # Ticks the spa and pushes state like WebServerHub::loop (about once a second).
    while True:
        spa.tick()
        message = json.dumps(spa.state_json())
        with Handler.clients_lock:
            clients = list(Handler.clients)
        for client in clients:
            try:
                send_ws(client, message)
            except (ConnectionError, OSError):
                with Handler.clients_lock:
                    if client in Handler.clients:
                        Handler.clients.remove(client)
        time.sleep(0.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1", help="0.0.0.0 to open it from your phone")
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--pumps", default="2,1",
                        help="speeds the board reports per pump, e.g. 2 or 2,1 (0 none, 1 on/off, 2 two-speed); "
                             "the default matches a BP100G2 set up for a pump 2 that isn't installed")
    parser.add_argument("--water", type=float, default=95, help="starting water temp F")
    parser.add_argument("--heat-rate", type=float, default=0.05, help="F per second while heating (fast, for demos)")
    parser.add_argument("--reminder", type=lambda v: int(v, 0), default=0, help="reminder code, e.g. 4 clean filter")
    parser.add_argument("--no-faults", action="store_true", help="start with an empty fault log")
    parser.add_argument("--no-filters", action="store_true", help="start with no filters, cartridges or water tests on the Maintenance tab")
    parser.add_argument("--location", default="39.74,-104.99",
                        help="lat,lon for outside temperature (default Denver); empty string for none")
    parser.add_argument("--no-heat-pump", action="store_true",
                        help="let pump 1 turn off while heating or filtering")
    args = parser.parse_args()

    Handler.spa = MockSpa(args)
    threading.Thread(target=run_simulation, args=(Handler.spa,), daemon=True).start()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    print(f"[mock] serving {DATA_DIR} at http://{args.host}:{args.port}  (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
