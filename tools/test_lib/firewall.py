"""Firewall (ACL) tests: GET → 4 lists, add probe rule into to_ap (the
operator-safest list — it filters traffic toward AP clients, so even an
accidental ALLOW probe rule can't lock us out via the management WiFi),
verify it appears, delete it, verify it's gone."""
from __future__ import annotations
from .common import Context, Result, SpaClient, check

MODULE_ID = "firewall"
MODULE_DESC = "ACL CRUD roundtrip on the to_ap list"

# to_ap = index 3 in this build (from_esp → AP clients in the ACL doc table)
# Cross-check with /api/firewall list[3].name; we don't hardcode the index
# strictly, we search by name to stay robust.
TARGET_LIST_NAME = "from_ap"  # safest: rules block clients FROM hitting the ESP,
                              # not management traffic toward the ESP


def _find_list_index(lists: list[dict], name: str) -> int | None:
    for entry in lists:
        if entry.get("name") == name:
            return int(entry.get("index", -1))
    return None


def run(ctx: Context) -> list[Result]:
    results: list[Result] = []
    spa = SpaClient(ctx)
    try:
        fw = spa.fetch_json("/api/firewall")
        check(results, MODULE_ID, "GET /api/firewall shape",
              isinstance(fw, dict) and isinstance(fw.get("lists"), list) and len(fw["lists"]) == 4,
              f"got {fw if not isinstance(fw, dict) else len(fw.get('lists', []))} lists")

        if not isinstance(fw, dict): return results

        # Stats non-negative on every list
        for lst in fw.get("lists", []):
            st = lst.get("stats", {})
            ok_stats = all(isinstance(st.get(k), (int, float)) and st.get(k, 0) >= 0
                           for k in ("allowed", "denied", "nomatch"))
            check(results, MODULE_ID, f"list {lst.get('name','?')}: stats non-negative", ok_stats,
                  f"stats={st}")

        idx = _find_list_index(fw["lists"], TARGET_LIST_NAME)
        if idx is None:
            check(results, MODULE_ID, f"locate list {TARGET_LIST_NAME!r}", False,
                  f"lists named: {[l.get('name') for l in fw['lists']]}")
            return results
        check(results, MODULE_ID, f"locate list {TARGET_LIST_NAME!r}", True)

        rules_before = len(fw["lists"][idx].get("rules", []))

        probe = {
            "acl":    idx,
            "src":    "10.255.255.0/24",   # unrouted: cannot affect any real flow
            "dest":   "10.255.254.0/24",
            "proto":  6,                    # TCP
            "s_port": 0,
            "d_port": 47222,
            "action": 0,                    # DENY (but src/dest never match real traffic)
            "monitor": False,
        }
        resp = spa.post_json("/api/firewall/add", probe)
        check(results, MODULE_ID, "add probe rule",
              isinstance(resp, dict) and resp.get("__http_status") == 200,
              f"resp={resp}")

        fw2 = spa.fetch_json("/api/firewall")
        rules_after = fw2["lists"][idx].get("rules", []) if isinstance(fw2, dict) else []
        check(results, MODULE_ID, "rule count grew by 1",
              len(rules_after) == rules_before + 1,
              f"before={rules_before} after={len(rules_after)}")
        probe_idx = len(rules_after) - 1
        added = rules_after[probe_idx] if rules_after else {}
        check(results, MODULE_ID, "added rule matches probe shape",
              added.get("src", "").startswith("10.255.255.") and added.get("d_port") == 47222,
              f"got {added}")

        # Delete the probe by its index
        resp_del = spa.post_json("/api/firewall/delete", {"acl": idx, "index": probe_idx})
        check(results, MODULE_ID, "delete probe rule",
              isinstance(resp_del, dict) and resp_del.get("__http_status") == 200,
              f"resp={resp_del}")

        fw3 = spa.fetch_json("/api/firewall")
        rules_final = fw3["lists"][idx].get("rules", []) if isinstance(fw3, dict) else []
        check(results, MODULE_ID, "rule count restored",
              len(rules_final) == rules_before,
              f"before={rules_before} final={len(rules_final)}")
        still_there = any(r.get("d_port") == 47222 and r.get("src", "").startswith("10.255.255.")
                          for r in rules_final)
        check(results, MODULE_ID, "probe not present after delete", not still_there)
    finally:
        spa.close()
    return results
