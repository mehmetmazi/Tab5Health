# Apple Health → Tab5 bridge (Cloudflare Worker)

Apple Health (HealthKit) has **no cloud API**, so blood pressure — which Hilo /
Aktiia writes into Apple Health — is bridged to the dashboard like this:

```
 iPhone (Health Auto Export)  ──POST──►  this Worker (KV store)  ──GET──►  Tab5
```

The Worker stores the latest reading and serves it back, gated by a shared token.
It also keeps the last raw payload (`?raw=1`) so you can inspect what the phone
actually sent.

## 1. Deploy the Worker

### Option A — Cloudflare dashboard (no CLI)
1. **Workers & Pages → Create → Worker.** Paste [`worker.js`](worker.js), deploy.
2. **Storage & Databases → KV → Create a namespace** (any name). Then in the
   Worker's **Settings → Bindings**, add a **KV namespace** binding named
   `HEALTH` pointing at that namespace.
3. **Settings → Variables and Secrets → add a Secret** `BRIDGE_TOKEN` = a long
   random string (your shared token). Deploy again.
4. Note the Worker URL, e.g. `https://tab5-health-bridge.<you>.workers.dev`.

### Option B — Wrangler CLI
```sh
npm i -g wrangler && wrangler login
wrangler kv namespace create HEALTH     # paste the returned id into wrangler.toml
wrangler secret put BRIDGE_TOKEN         # paste your random token
wrangler deploy
```

## 2. Configure Health Auto Export (iPhone)
1. **Automations → add a REST API automation.**
2. **URL** = your Worker URL · **Method** = `POST` · **Format** = `JSON`.
3. **Header:** `Authorization: Bearer <your BRIDGE_TOKEN>`.
4. **Data:** select **Blood Pressure** (add others later if you want).
5. **Schedule:** hourly, or on new data.

## 3. Test
```sh
TOKEN=your-bridge-token
URL=https://tab5-health-bridge.you.workers.dev

# latest parsed reading (this is what the Tab5 reads):
curl -H "Authorization: Bearer $TOKEN" "$URL"
#   -> {"systolic":120,"diastolic":80,"ts":"2026-06-26 14:30:00 +0000","updated":"..."}

# raw last payload from Health Auto Export (debug the parse):
curl -H "Authorization: Bearer $TOKEN" "$URL?raw=1"
```

If the parsed reading is empty but `?raw=1` shows data, the metric shape differs
from the documented one — send me the raw JSON and I'll adjust `extractBP()`.

## 4. On the Tab5 (next step)
A provider (`components/providers/applehealth.cpp`) GETs `$URL` with the same
token and shows a **Blood Pressure** tile; its URL + token live in a gitignored
`main/applehealth_credentials.h`. That firmware side is built once the Worker is
live and returning data.

> ⚠️ Blood pressure is sensitive health data. Keep `BRIDGE_TOKEN` secret so the
> endpoint isn't open to the world, and prefer a long random value.
