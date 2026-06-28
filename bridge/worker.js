// Cloudflare Worker — Apple Health -> Tab5 bridge relay.
//
//   POST  (from Health Auto Export):  stores the latest blood-pressure reading
//   GET   (from the Tab5 firmware):   returns the latest reading as small JSON
//
// Both require the shared secret, sent as `Authorization: Bearer <token>`
// (or `?token=<token>`). Bindings, set in the Cloudflare dashboard / wrangler:
//   - KV namespace bound as  HEALTH
//   - secret / variable      BRIDGE_TOKEN

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const want = env.BRIDGE_TOKEN || "";
    const got =
      (request.headers.get("authorization") || "").replace(/^Bearer\s+/i, "") ||
      url.searchParams.get("token") ||
      "";
    if (!want || got !== want) return json({ error: "unauthorized" }, 401);

    if (request.method === "POST") {
      const raw = await request.text();
      // keep the last raw payload for debugging the parse (?raw=1 on GET)
      await env.HEALTH.put("bp_raw", raw, { expirationTtl: 60 * 60 * 24 * 30 });
      let rec = null;
      try { rec = extractBP(JSON.parse(raw)); } catch (_) {}
      if (rec) await env.HEALTH.put("bp", JSON.stringify(rec));
      return json({ ok: true, parsed: rec });
    }

    if (request.method === "GET") {
      const key = url.searchParams.has("raw") ? "bp_raw" : "bp";
      const v = await env.HEALTH.get(key);
      return new Response(v || "{}", {
        headers: { "content-type": "application/json" },
      });
    }

    return json({ error: "method not allowed" }, 405);
  },
};

// Newest {systolic, diastolic} across every Health Auto Export metric's data
// points. Name-agnostic: BP points carry both fields (and no `qty`), so we just
// look for points that have both and keep the latest by timestamp.
function extractBP(j) {
  const metrics = j?.data?.metrics || j?.metrics || [];
  let best = null;
  for (const m of metrics) {
    for (const p of m.data || []) {
      if (p && p.systolic != null && p.diastolic != null) {
        if (!best || String(p.date) > String(best.date)) best = p;
      }
    }
  }
  if (!best) return null;
  return {
    systolic: Math.round(Number(best.systolic)),
    diastolic: Math.round(Number(best.diastolic)),
    ts: best.date || null,
    updated: new Date().toISOString(),
  };
}

function json(obj, status = 200) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json" },
  });
}
