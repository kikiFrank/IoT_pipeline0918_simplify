#!/bin/bash
# Enable aibox-rule_engine status_update (conf=0.5) and patch cam-001 zone.
set -euo pipefail
ENV_FILE=/etc/aibox/rule_engine.env
ZONE_ID=4d1a7906-2beb-4f0b-bff9-aa84aea57f6e
WEB=https://127.0.0.1:8444
USER=admin
PASS=Aibox12345678

echo "=== patch $ENV_FILE ==="
sudo cp -a "$ENV_FILE" "${ENV_FILE}.bak.$(date +%Y%m%d%H%M%S)"
python3 - <<'PY'
from pathlib import Path
src = Path("/etc/aibox/rule_engine.env")
dst = Path("/tmp/rule_engine.env.new")
text = src.read_text()
wanted = {
    "AIBOX__STATUS_UPDATE__ENABLED": "true",
    "AIBOX__STATUS_UPDATE__CONFIDENCE_THRESHOLD": "0.5",
    "AIBOX__STATUS_UPDATE__PROBLEM_DURATION_SECS": "3.0",
    "AIBOX__STATUS_UPDATE__RECOVERY_DURATION_SECS": "2.0",
    "AIBOX__STATUS_UPDATE__COOLDOWN_SECS": "5.0",
    "AIBOX__LEGACY_FRAME_WIDTH": "640",
    "AIBOX__LEGACY_FRAME_HEIGHT": "360",
}
lines = []
seen = set()
for raw in text.splitlines():
    if not raw.strip() or raw.lstrip().startswith("#") or "=" not in raw:
        lines.append(raw)
        continue
    key = raw.split("=", 1)[0].strip()
    if key in wanted:
        lines.append(f"{key}={wanted[key]}")
        seen.add(key)
    else:
        lines.append(raw)
for key, val in wanted.items():
    if key not in seen:
        lines.append(f"{key}={val}")
dst.write_text("\n".join(lines) + "\n")
print("ENV_STAGED")
print(dst.read_text())
PY
sudo cp /tmp/rule_engine.env.new "$ENV_FILE"
sudo chmod 640 "$ENV_FILE"
sudo chown root:ubuntu "$ENV_FILE"
echo ENV_OK
cat "$ENV_FILE"

echo "=== restart aibox-rule_engine ==="
sudo systemctl restart aibox-rule_engine
sleep 2
systemctl is-active aibox-rule_engine
journalctl -u aibox-rule_engine -n 20 --no-pager

echo "=== login local-web ==="
TOKEN=$(python3 - <<PY
import json, ssl, urllib.request
ctx = ssl._create_unverified_context()
req = urllib.request.Request(
    "$WEB/auth/login",
    data=json.dumps({"username": "$USER", "password": "$PASS"}).encode(),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urllib.request.urlopen(req, context=ctx, timeout=10) as resp:
    body = json.loads(resp.read().decode())
print(body.get("token") or body.get("access_token") or "")
PY
)
if [ -z "$TOKEN" ]; then
  echo LOGIN_FAIL
  exit 1
fi
echo LOGIN_OK

echo "=== patch zone $ZONE_ID ==="
python3 - <<PY
import json, ssl, urllib.request
ctx = ssl._create_unverified_context()
patch = {
    "points": [
        {"x": 0.0, "y": 0.0},
        {"x": 1.0, "y": 0.0},
        {"x": 1.0, "y": 1.0},
        {"x": 0.0, "y": 1.0},
    ],
    "enabled": True,
    "param_overrides": {
        "enabled": True,
        "problem_classes": ["Red", "Red-green"],
        "confidence_threshold": 0.5,
        "problem_duration_secs": 3.0,
        "recovery_duration_secs": 2.0,
        "cooldown_secs": 5.0,
    },
}
req = urllib.request.Request(
    "$WEB/zones/$ZONE_ID",
    data=json.dumps(patch).encode(),
    headers={
        "Content-Type": "application/json",
        "Authorization": "Bearer $TOKEN",
    },
    method="PATCH",
)
with urllib.request.urlopen(req, context=ctx, timeout=10) as resp:
    body = json.loads(resp.read().decode())
print(json.dumps({
    "zone_id": body.get("zone_id"),
    "camera_id": body.get("camera_id"),
    "enabled": body.get("enabled"),
    "action_types": body.get("action_types"),
    "param_overrides": body.get("param_overrides"),
    "points": body.get("points"),
    "zone_revision": body.get("zone_revision"),
}, indent=2))
PY

echo "=== zones.json snapshot ==="
python3 - <<'PY'
import json
from pathlib import Path
p = Path("/var/lib/aibox/rule-engine/zones.json")
data = json.loads(p.read_text())
print("config_version", data.get("config_version"))
for z in data.get("zones", []):
    if z.get("camera_id") == "cam-001":
        print(json.dumps({
            "zone_id": z["zone_id"],
            "name": z["name"],
            "action_types": z["action_types"],
            "enabled": z["enabled"],
            "param_overrides": z.get("param_overrides"),
            "points": z.get("points"),
        }, indent=2))
PY
echo APPLY_DONE
