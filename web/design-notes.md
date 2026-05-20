# Design Notes — ESP32 Tailscale Subnet Router Web UI

## Aesthetic Direction

**Domain:** Embedded device admin dashboard. Not editorial, not marketing — operational.

**Override:** Explicitly overrides the Opus 4.7 default house style (cream/serif/terracotta). That aesthetic is wrong for a router admin panel.

**Chosen direction:** Linear / Cloudflare Dashboard

| Token | Value | Rationale |
|-------|-------|-----------|
| Page bg | `#0d0f12` | Near-black, not pure black — avoids harsh contrast, matches modern dev tools |
| Card bg | `#161a1f` | One tier up from page, clearly distinct without gradients |
| Elevated card | `#1e2329` | For inputs, hover states, nested containers |
| Accent | `#3b82f6` | Tailwind blue-500 — recognisable, professional, not the existing cyan glow |
| Success | `#22c55e` | Flat green, no gradient |
| Warning | `#f59e0b` | Amber, matches severity conventions |
| Danger | `#ef4444` | Red, matches severity conventions |
| Info | `#06b6d4` | Cyan kept for informational only (not accent), echoes existing brand |

**What was deliberately avoided:**
- `linear-gradient(135deg, #667eea, #764ba2)` — the existing purple gradient buttons look like generic Tailwind UI examples, not a custom tool
- `text-shadow: 0 0 20px rgba(0,217,255,0.3)` — the cyan glow heading style, which reads as dated "cyberpunk dark mode"
- `backdrop-filter: blur(10px)` glassmorphism — attractive in screenshots, causes GPU compositing overhead on mobile Safari and contributes nothing to usability

**Typography:**
System font stack (`-apple-system, 'Segoe UI', system-ui, sans-serif`) for UI chrome — no CDN font load, zero latency on first paint. Monospace stack (`'SF Mono', 'Geist Mono', 'Cascadia Code', ui-monospace, monospace`) for IPs, MACs, CIDRs, and log output. These are credentials the user needs to read and copy accurately — monospace is functional, not decorative.

**The one memorable detail:** Left-border accent stripes on cards convey severity/state at a glance without colour flooding the whole card. A green-left-striped uplink card and a red-left-striped heap card communicate status before the user reads a single label. This is borrowed from VS Code's problem list and GitLab's pipeline status — it works because it is spatially anchored.

---

## Why No Framework

### Why not React / Vue / Svelte

The ESP32 HTTP server serves a single static file. Build tooling would require a separate host-side compile step, a `dist/` artifact, and a deployment procedure. The target developer (Csontikka, or any contributor) should be able to edit the HTML file and `idf.py flash` — no `npm install` required.

Vanilla JS is sufficient for:
- Tab navigation (7 lines)
- `fetch('/api/status')` polling (30 lines)
- DOM patching on response (the `applyStatus()` function)
- Toast system (~20 lines)
- Confirm modal (~15 lines)

Total JS in the prototype: ~250 lines. A React app doing the same thing would be 50KB+ of runtime before a single line of application code.

### Why not Tailwind CDN

The CDN version ships ~3.5MB of CSS (all utilities). Even the Play CDN with JIT is ~300KB. The production build constraint is <100KB gzip total. Tailwind is the wrong tool when you control the entire design token surface anyway — CSS custom properties achieve the same consistency without the dependency.

---

## Navigation Pattern

**Top nav with pill-shaped active tab** — chosen over:

- Sidebar: wastes horizontal space on mobile; requires a hamburger overlay mechanism; common on desktop apps but friction-heavy for first-time setup on a phone
- Bottom nav (mobile): familiar on consumer apps, but admin tools are not consumer apps; bottom nav implies ≤5 items with icons only — the 6-item set with labels doesn't fit cleanly
- Page-per-URL multi-page: would require the ESP32 to serve 6+ separate HTML files, adding RAM/flash pressure; SPA avoids that

The tab strip scrolls horizontally on mobile with `overflow-x: auto; scrollbar-width: none` — tabs never wrap or truncate, and the active tab is always visible because the JS scrolls it into view on activation.

**data-page attribute routing:** Each `.nav-tab` has `data-page="section-id"`. The JS listener toggles `.active` on both the tab and the matching `#page-{section}` div. No router library needed. Deep linking is not required (the device has no persistent URL state worth bookmarking).

---

## Performance Budget

Measured on the prototype (uncompressed / estimated gzip):

| Asset | Raw | Gzip estimate |
|-------|-----|---------------|
| CSS (design tokens + components) | ~14 KB | ~3.5 KB |
| HTML structure (all 6 page sections) | ~12 KB | ~3 KB |
| JavaScript (nav + fetch + DOM updates) | ~8 KB | ~2.5 KB |
| Inline SVG icons (~20 icons at ~150 bytes each) | ~3 KB | ~1 KB |
| **Total prototype** | **~37 KB** | **~10 KB** |

Target for full production build (all 6 pages fully implemented):

| Phase | Raw estimate | Gzip estimate |
|-------|-------------|---------------|
| Status + nav chrome | ~40 KB | ~11 KB |
| + Network page (forms + tables) | +10 KB | +3 KB |
| + Tailscale page | +8 KB | +2.5 KB |
| + Firewall page | +6 KB | +2 KB |
| + Tools page | +8 KB | +2.5 KB |
| + System page | +8 KB | +2.5 KB |
| **Full SPA total** | **~80 KB** | **~24 KB** |

Well within the 100 KB gzip budget. The ESP32 SPIFFS/LittleFS partition is typically 1–4 MB so raw size is fine too.

**Optimisation levers if needed:**
1. Remove stub page HTML from prototype — production pages render their sections lazily via JS template strings rather than static HTML
2. SVG icons: deduplicate with `<use href="#icon-id">` and a hidden `<svg>` sprite at top of body — saves ~2 KB
3. Minify CSS + JS with a one-shot Python/Node script in the build pipeline — no Webpack required

---

## Component Vocabulary Reference

All components are pure CSS classes. No JS required except for interactive components (modal, toast, password toggle).

```
Layout
  .nav                    Top navigation bar (sticky, 48px)
  .nav-tab[data-page]     Navigation tab button
  .nav-tab.active         Active tab (pill background)
  .page-container         Max-width content wrapper
  .page-section           Tab content panel (display:none by default)
  .page-section.active    Visible panel
  .page-header            Title + subtitle + action row
  .grid.grid-cols-{1|2|3} Responsive card grid
  .col-span-{2|3}         Grid column span

Cards
  .card                   Default card (bg-card, border, radius-xl)
  .card-elevated          Elevated variant (bg-elevated)
  .card-stripe-{success|warning|danger|info|accent}  Left border accent
  .card-header            Card title row (with separator line)
  .card-title             Section label (uppercase, muted, small)
  .card-body              Padded card content
  .card-body-flush        No padding (for tables that need full-width rows)

Data display
  .stat-list              Container for stat rows
  .stat-row               Label + value row (with hover, bottom border)
  .stat-label             Left label (muted, fixed min-width)
  .stat-value             Right value (primary text)
  .stat-value.mono        Monospace value (IPs, MACs)
  .metric-card            Large KPI display (value + label + sub)
  .metric-value           Large number (1.75rem, tabular nums)
  .heap-bar-track/.heap-bar-fill  Horizontal progress bar

Tables
  .table-wrap             Horizontal scroll container
  .table                  Data table (hover rows, header style)
  .table-zebra            Optional zebra striping

Badges
  .badge                  Base badge (inline-flex, pill)
  .badge-{success|warning|danger|info|accent|neutral}  Semantic variant
  .badge-dot              Circular dot inside badge
  .badge-dot-pulse        Animated pulse (for "live" states)

Buttons
  .btn                    Base button
  .btn-{sm|md|lg}         Size variants
  .btn-icon               Square icon-only button
  .btn-{primary|secondary|ghost|danger}  Style variants
  .btn-full               Full width

Inputs
  .form-group             Vertical label + input stack
  .form-label             Input label
  .form-hint              Helper text below input
  .input                  Text/number/password input
  .input-mono             Monospace input (for IPs, CIDRs)
  .input-with-icon        Wrapper for icon-inside-input
  .select                 Styled select element
  .textarea               Resizable textarea
  .check-group            Checkbox + label pair
  .radio-group            Radio + label pair

Signal bars
  .signal-bars            Container (inline-flex, 14px tall)
  .signal-bars .bar       Individual bar (4 per instance)
  .bar.on-{excellent|good|fair|weak|poor}  Active bar color

Feedback
  .badge / .badge-dot-pulse   Inline status indicator
  .toast-container        Fixed bottom-right toast stack
  .toast.toast-{success|warning|error|info}  Toast notification
  .alert.alert-{...}      Inline alert block
  .empty-state            Centred empty state (icon + heading + caption)
  .loading-state          Spinner + text row
  .spinner                CSS keyframe spinner

Forms
  .form-footer            Sticky bottom bar for Save/Cancel
  .form-footer-hint       Left-side hint text in footer
  .form-footer-actions    Right-side button group

Modals
  .modal-backdrop         Full-screen overlay (display:none by default)
  .modal-backdrop.open    Visible overlay
  .modal                  Modal box (animated scale-in)
  .modal-header/.modal-body/.modal-footer  Modal layout slots
```

---

## API Contract Assumptions

The JS `applyStatus()` function expects `GET /api/status` to return:

```json
{
  "uplink": {
    "connected": true,
    "ssid": "HomeNetwork_5G",
    "ip": "192.168.1.47",
    "rssi": -62,
    "channel": 6,
    "band": "2.4 GHz"
  },
  "ap": {
    "ssid": "ESP32_IoT_AP",
    "ip": "192.168.4.1",
    "mac": "7C:DF:A1:5E:3B:02",
    "channel": 6,
    "clients": 3
  },
  "heap_free": 191488,
  "heap_total": 258048,
  "uptime_sec": 188820,
  "last_reboot": "2026-05-18T09:41:00",
  "reset_reason": "POWER_ON",
  "tailscale": {
    "enabled": true,
    "connected": true,
    "ip": "100.64.0.12",
    "hostname": "esp32-router",
    "routes": ["192.168.4.0/24"],
    "exit_node": null,
    "peers_online": 4,
    "peers_total": 6,
    "peers": [
      {"name": "laptop-work", "ip": "100.64.0.1", "online": true, "path": "direct"},
      {"name": "pi-homelab",  "ip": "100.64.0.3", "online": true, "path": "DERP"}
    ]
  },
  "telemetry": {
    "status": "ok",
    "boot_count": 47,
    "flash_count": 12
  }
}
```

**`reset_reason` string values expected:** `POWER_ON`, `SW_RESET`, `PANIC`, `WDT`, `BROWNOUT`, `FLASH`, `DEEP_SLEEP`

These map to badge colours in `resetReasonDisplay()`. If the C backend uses numeric codes instead, add a translation table in JS — do not change the JS/CSS architecture.

---

## Future-Proofing Notes

### Adding a new page section

1. Add a `.nav-tab` button with `data-page="new-section"` in the nav
2. Add a `<div class="page-section" id="page-new-section">` in the container
3. The JS nav router picks it up automatically — no JS changes needed
4. Add the page's wireframe to `wireframes/` and update this doc

### Adding crash info display

When `reset_reason` is `PANIC` or `WDT`, show an additional `.card.card-stripe-danger` below the uptime card on the Status page. The card renders: task name, PC address, backtrace frames. Backend should add a `crash` object to the `/api/status` response:

```json
"crash": {
  "task": "tiT",
  "pc": "0x400d1234",
  "backtrace": ["0x400d1234", "0x400d5678", "0x400e1abc"]
}
```

The JS `applyStatus()` function checks `data.crash` and conditionally renders this card. When null/absent, card is not shown.

### Internationalisation

No i18n framework is needed now. If Hungarian labels are ever wanted:
- Store all UI strings in a `const STRINGS = {}` object at the top of the script
- Replace hardcoded English strings with `STRINGS.foo` references
- Swapping the object switches language — no framework, no bundle size increase

### Replacing hardcoded peers list with a proper table

The current prototype renders peers as `.peer-row` divs. When the peer count grows or columns are added (RTT, last-seen), switch to a proper `.table` inside `.card-body-flush`. The `.table` component is already defined — it is a CSS swap, not a component rewrite.

### Dark/light mode toggle

All colours are CSS custom properties on `:root`. A light mode variant would override ~15 variables inside a `[data-theme="light"]` selector. The JS toggle sets `document.documentElement.dataset.theme = 'light'` and saves to localStorage. No CSS architecture change required.

---

## Open Questions for Csontikka

### 1. Crash info card placement and trigger
The current Status page shows `reset_reason` as a badge in the Uptime card. When reason is PANIC or WDT, should the crash detail (task + PC + backtrace) appear:
- **Option A:** Inline expansion inside the Uptime card (click "Show crash details")
- **Option B:** A separate `.card.card-stripe-danger` inserted between Uptime and Telemetry
- **Option C:** A banner alert at the very top of the Status page (most visible, hard to miss)

Option C is the most actionable for a developer but most intrusive. What is your preference?

### 2. Nav label visibility on mobile
At 360px, the nav fits 4–5 tabs before needing to scroll. The current design hides the brand name but keeps all tab labels. Two alternatives:
- **Option A (current):** Keep all labels, let the tab row scroll horizontally (hidden scrollbar)
- **Option B:** Icon-only tabs on mobile (≤480px), labels on tablet+ — saves space but requires all 6 icons to be unambiguous without text
- **Option C:** Hamburger menu on mobile that opens a slide-in drawer

Option B is the cleanest on a 360px screen — but only if you are happy with the SVG icon choices. Worth testing on a real phone before committing.

### 3. Auto-refresh interval on Status page
The prototype polls `/api/status` every 30 seconds. For a router admin panel this is reasonable at idle, but during active setup (e.g. watching Tailscale connect) 30s feels slow.
- Keep 30s as the default?
- Add a manual "live mode" toggle that drops to 3s polling while active?
- Or use SSE (Server-Sent Events) for push updates — the ESP32 HTTP server supports chunked transfer, so SSE is feasible without WebSocket complexity.

SSE would make the Status page feel instant and eliminate polling entirely, but adds ~50 lines of JS and a new `/api/events` endpoint on the C side.

### 4. Tailscale page layout split
The prototype puts the Tailscale config form and live peers table side-by-side on desktop. An alternative is two sub-tabs within the Tailscale page: "Configuration" and "Peers". The sub-tab approach is cleaner on tablet where the 2-col layout is tight, but adds a navigation layer.
- **Option A (current):** Side-by-side cards on desktop, stacked on mobile
- **Option B:** Sub-tabs within the Tailscale page (Configuration / Peers)

### 5. Inline add vs modal for DHCP reservations and port mappings
The Network page wireframe specifies an inline-row add pattern (a new editable row appends to the table). An alternative is a small modal form. The inline approach is faster but tricky to implement accessibly on mobile (the row may be off-screen). The modal approach is more standard but adds a click.
- Which pattern feels right for the project's UX bar?
- Note: the Firewall page uses the same pattern — the decision should be consistent across both pages.
