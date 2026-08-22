# Wireless Link Play, Matchmaking, and Console Clustering

> Design research, 2026-08-22. Nothing here is built yet.
> Depends on `docs/21-PIO-PORT-REFERENCE.md` (PIO port hardware) and
> `docs/13-SIO1-HARDWARE-RESEARCH.md` (serial port).

---

## 1. The governing insight

**The console has no idea any of this exists.**

A link-cable game writes to the SIO1 registers at `0x1F801050–105E` and expects
another console on the other end of a wire. It has no concept of WiFi, lobbies,
accounts or the expansion port. Everything we build lives *outside* the console's
awareness.

That single fact decides the whole architecture:

- All identity, lobby and routing logic lives in the **Pico + server**, never on
  the console.
- The wireless link carries the **serial byte stream**, not the electrical
  timing. Each Pico handles its own console's signalling locally; the network in
  between is a dumb byte pipe. The two sides never have to agree on baud rate
  over the network — they each measure their own console independently.

A corollary worth stating early: **existing games cannot be moved onto the PIO
port.** They talk to SIO1 and only SIO1. PIO is for software we write ourselves
(or for a cart that patches a game's serial routines in RAM at boot — real, but
per-title reverse engineering).

---

## 2. Latency: what is actually possible

Link games typically exchange data **once per frame** — 16.7 ms on NTSC, 20 ms on
PAL. That is the entire budget.

| Route | Approx. RTT | Verdict |
|---|---|---|
| Same room / LAN | 1–3 ms | Works. Effectively a cable. |
| Sydney ↔ Melbourne | ~15 ms | Inside one frame. Should be fine. |
| Australia ↔ Singapore | ~100 ms | RTS plausible; racing no. |
| Australia ↔ Japan | ~130 ms | Same. |
| **Australia ↔ France** | **~280–320 ms** | ~18 frames over budget. Only if the game buffers commands. |

Sydney–Paris is ~17,000 km; at ~200,000 km/s in fibre that is ~170 ms RTT from
physics alone, before routing. There is no engineering under that floor.

### Why genre decides it

- **Racing / action** (Wipeout, Ridge Racer Revolution) — frame-locked. Dead
  beyond a few tens of ms.
- **RTS** (Command & Conquer) — the best candidate on the console. Classic RTS
  netcode is *deterministic lockstep with a command-delay buffer*: it transmits
  orders, not state, and both machines simulate identically N frames later. That
  is how the genre shipped playable over 28.8k modems. If the PS1 port uses a
  command-delay model with a deep enough buffer, 300 ms is survivable — you issue
  an order and it resolves a beat later, which for an RTS is acceptable.

**Unknown:** whether the PS1 port hard-blocks on the serial handshake each frame,
or buffers. This is the single question that decides the France experiment, and it
is answerable on a bench.

### The experiment that answers it — build this first

Two consoles, one room, a bridge that **injects artificial delay**. Sweep
5 → 20 → 50 → 100 → 200 → 300 ms and find each game's breaking point.

Cheap, needs no international coordination, and produces a *table* of how far away
each game is playable — far more interesting than a single pass/fail at one
distance.

---

## 3. Matchmaking and routing

### 3.1 Topology

```
Console A --SIO1--> Pico W (A) --WiFi--> \
                                          Match server / relay
Console B --SIO1--> Pico W (B) --WiFi--> /
```

### 3.2 Identity

Each Pico has a stable device ID (RP2040 flash UID) bound to a user account.
Provisioning happens through the Pico's own captive-portal web UI — the same
pattern BlueRetro already uses for its configuration page: join WiFi, enter
credentials, link the account.

### 3.3 Flow

1. **Lobby lives on the web, not the console.** Player opens the site on a phone
   or PC, sees who is online, picks an opponent, hits connect.
2. **Server pairs the two Picos** and assigns roles — link setups have a
   master/slave or host/guest asymmetry, so the server decides it at pairing time
   and tells both ends.
3. **Transport is established**, preferring direct peer-to-peer UDP via hole
   punching, falling back to a server relay when NAT is hostile.
4. **Only then do the players start the game** and choose versus/link mode. The
   Picos bridge serial bytes transparently from that point on.

Order matters: pair *first*, then bring up the console-side link. Link games probe
the serial line and only offer versus mode if they detect a peer, so the pairing
must already be live when the game looks.

### 3.4 The hard part — underrun policy

Serial has no framing and no natural flow control here. If one side runs slightly
fast, the elastic buffer drains. What do we hand the console when no data has
arrived?

| Option | Consequence |
|---|---|
| **Stall** — hold off the serial handshake | Game waits. Manifests as slowdown. Correct but visible. |
| Send idle/last-known bytes | Smooth, but risks **desync**, which is unrecoverable |

**Recommendation: stall**, and let the game's own timeout handling deal with it. A
desynced RTS is worse than a slow one.

### 3.5 Two lobby models

| | Commercial link games | Our homebrew |
|---|---|---|
| Lobby | External — web/phone. The game can't be told about it. | **On-console.** Our code can query the Pico directly. |
| Transport | SIO1 bridge | PIO — far faster |

For anything we write, the lobby can live on the PlayStation itself, because our
software is allowed to know the Pico exists.

### 3.6 Gotchas

- **Baud detection.** The game picks the baud rate and it varies per title. Each
  Pico must measure its own console's bit timing, or ship per-game profiles.
- **Region mismatch.** PAL is 50 Hz, NTSC 60. Two consoles ticking at different
  rates will break link code that assumes a shared frame clock. Match regions for
  the first attempt.
- **PSone has neither port.** No SIO1, no PIO. Not reproducible there.

### 3.7 Redirecting SIO1 to the controller port (SIO0)

Idea: instead of putting hardware on the serial port, have a PIO cart take over at
boot (`docs/21` §7, two hook points) and redirect the game's serial traffic to
**SIO0** — the controller port — where a BlueRetro-class ESP32 already sits doing
Bluetooth and WiFi. One device instead of two.

**The generic mechanism — hardware data breakpoints, not per-game patches.**
The R3000A's COP0 has breakpoint registers: `BPC` (execute), `BDA` (data address),
`BDAM`/`BPCM` (masks) and `DCIC` (control). Set a data breakpoint over the SIO1
register range `0x1F801050–105E` and every access traps to our handler, which
performs the equivalent SIO0 transaction instead. **No knowledge of the game is
required.** This is the same facility cheat devices use.

Cost is one exception per SIO1 register access. Link games touch those registers
only a handful of times per frame, so the overhead is negligible.

**Bandwidth is not a problem.** SIO0 runs at ~250 kHz; link games typically pick
baud rates from 9,600 up to ~115,200. The controller port is comfortably faster
than what it is replacing.

**Two real risks:**

1. **Register semantics.** The handler must emulate SIO1's status bits — TX ready,
   RX ready, IRQ behaviour — convincingly enough that the game's polling loops
   behave. That is the bulk of the work and needs care, not cleverness.
2. **Sharing SIO0 with controller polling.** The game still reads the pad every
   frame over the same bus. Our handler must complete its SIO0 transaction
   atomically — interrupts masked, SIO0 state saved and restored — or it will
   corrupt an in-flight pad transfer.

**On an original PS1 this is the hard way round.** The game already talks to SIO1;
putting a Pico or ESP32 directly on SIO1 needs no cart, no exception handler and no
register emulation. Prefer that.

**Where it genuinely wins: the PSone.** The PSone has *no serial port at all*, so
redirecting to the controller port is the only way those consoles could ever do
link play. Note the PSone also has no expansion port, so the patch cannot come from
a cart — inject it with the FreePSXBoot memory-card exploit instead.

That is the compelling version of this idea: **link play on hardware Sony shipped
without the capability**, rather than doing it the awkward way on a machine that
already had the port.

### 3.8 Link-cable game list, ranked by latency tolerance

The cable is the **SCPH-1040**, an 8-pin inline serial lead between the two
consoles' serial I/O ports. Two consoles, two TVs, two copies of the game —
the point being full-screen play instead of split-screen.

Titles below are from the Wikipedia list (see Sources). The **tolerance** column
is our own prediction from genre and expected netcode, not measured — it is
exactly what the delay-injection rig exists to confirm or demolish.

#### Tier A — best candidates (command-style netcode, latency tolerant)

| Game | Notes |
|---|---|
| Command & Conquer: Red Alert | RTS. Best long-distance candidate on the console. |
| C&C: Red Alert — Retaliation | RTS |
| Dune 2000 | RTS |

RTS traditionally ships *orders*, not state, resolved N frames later. This is the
one genre with a real chance at 300 ms. **Test these first.**

#### Tier B — plausible (slower pace, may buffer)

| Game | Notes |
|---|---|
| Armored Core / Project Phantasma / Master of Arena | 3rd-person mech combat |
| Metal Jacket (JP), Mobile Suit Z Gundam (JP) | |
| Bogey Dead 6, Independence Day, Wing Over, Zero Pilot (JP) | flight/combat sim |
| Pro Pinball: Big Race USA | likely turn-based exchange |
| Robo Pit 2, Blast Radius | |

#### Tier C — expect failure past a few tens of ms (frame-locked action)

| Game | Notes |
|---|---|
| Descent, Descent Maximum, Doom, Final Doom | FPS |
| Duke Nukem: Total Meltdown, Krazy Ivan | FPS |
| Assault Rigs, Destruction Derby, Red Asphalt | vehicular combat |
| Rogue Trip: Vacation 2012 | |
| Twisted Metal 3 | NA, 2–8 players |
| Bushido Blade, Bushido Blade 2 | fighting — the least forgiving genre of all |
| Cool Boarders 2, Trick'N Snowboarder, Streak: Hoverboard Racing | |

#### Racing — all Tier C, 2–4 players unless noted

Andretti Racing · Ayrton Senna Kart Duel · Burning Road · CART World Series ·
Dead in the Water · Dodgem Arena (PAL) · Explosive Racing · Formula 1 ·
Formula 1 98 · Monaco Grand Prix · Motor Toon Grand Prix 2 ·
R4: Ridge Racer Type 4 · Racingroovy (JP) · Ridge Racer Revolution ·
Road & Track Presents: The Need for Speed · San Francisco Rush: Extreme Racing ·
Shutokou Battle R (JP) · Test Drive 4 · Test Drive Off-Road (NA) ·
TOCA 2 Touring Cars · Total Drivin · Wipeout · Wipeout 2097 ·
Wipeout 3 (2–4) · Wipeout 3: Special Edition (PAL, 2–4)

**Suggested test set for the delay sweep:** Red Alert (Tier A), Armored Core
(Tier B), Wipeout 2097 (Tier C). Three points on the curve is more informative
than twenty runs of the same genre — and a title that fails at 50 ms is as good
for the video as one that survives 300.

Note several are 2–4 or even 2–8 player, so the transport design should not
hard-assume exactly two endpoints.

### 3.9 Can button monitoring help keep games synced?

Since BlueRetro *supplies* the controller data, both players' inputs pass through
devices we control. Tempting — but be clear about what is and is not possible.

**True rollback netcode is impossible on real PS1 hardware.** GGPO-style rollback
needs two things we cannot have:

1. **Snapshot and restore full game state** every frame — 2 MB of main RAM plus
   VRAM, SPU RAM and register state. Not at 60 Hz, not over any link we have.
2. **Re-simulate N frames within one frame** — that needs the console to run at
   several times realtime. It runs at 1×. There is no way around this.

**Naive input prediction breaks determinism.** Substituting "repeat last input"
for a late peer works only if *both* consoles substitute identically. But the
remote console has its own player pressing real buttons at that instant, so one
side applies real input and the other applies predicted — and lockstep
simulations diverge. A desync is unrecoverable; this is strictly worse than
stalling.

**What button monitoring is genuinely worth doing:**

- **Protocol identification — the high-value use.** Watch the button state on
  SIO0 alongside the payload the game pushes over SIO1. If the serial traffic is
  small and correlates tightly with button changes, the game ships *inputs* →
  command-style netcode → latency tolerant. If it is large, continuous, and
  uncorrelated, it ships *state* → frame-locked → hopeless at distance.
  **This tells us a game's latency tolerance without a second console**, and it
  turns the Tier A/B/C guesses above into measurements.
- **Desync diagnostics.** Logging inputs against serial traffic gives a timeline
  to inspect when a session falls apart.
- **Pad-injected pause as a hiccup handler.** Because we control the pad, a
  sustained network stall could inject Start on both consoles to pause, then
  resume — visible and recoverable, rather than a silent desync. Crude, and it
  still needs both sides to pause near the same frame, but worth prototyping
  against stalling the serial line, which may trip a game's own timeouts.

**Summary: monitoring buys us understanding and diagnostics, not synchronisation.**
Sync has to come from the game's own netcode being latency-tolerant, which is why
identifying which games those are is the first job.

### 3.10 Prior art

Transparent link tunnels are well-trodden on other consoles — original Xbox
(XLink Kai and similar), GBA, Dreamcast, N64. The pattern is proven; nobody has
done it on the PS1 parallel port with a modern microcontroller.

---

## 4. Can two consoles work *together*? (clustering)

Short answer: **useless for real work, excellent as a demo.** Worth knowing why.

### 4.1 The ratio that kills it

| | |
|---|---|
| R3000A | 33.87 MHz, roughly 30 MIPS |
| Local RAM bandwidth | tens of MB/s |
| PIO interconnect | a few MB/s at best |
| SIO1 interconnect | ~11–50 KB/s |

Even over PIO you get roughly **one byte moved per ~10 CPU cycles**. Distributed
computing only pays when the compute-to-communication ratio is enormous — here it
needs to be in the thousands to one before a second console adds anything.

Two further blockers:

- **No MMU.** Blackroo is nommu, so distributed shared memory is not merely slow,
  it is impossible — pointers are physical addresses.
- **Latency, not just bandwidth.** Every exchange costs a round trip through a
  narrow port.

### 4.2 What genuinely works

Only embarrassingly parallel work — split it once, compute for a long time, return
a small result:

- **Mandelbrot / fractal rendering** — split by scanline. Near-zero communication.
- **Raytracing** — same.
- **Brute-force search** (hashes, chess perft, key spaces) — partition the space.
- **Distributed build/compile** across blackroo nodes — plausible, and it looks
  great even though a laptop would beat it by four orders of magnitude.

### 4.3 What does not

- **Splitting video decode** across consoles. Decoding frame N on the other box
  means shipping a *decoded* frame back — 150 KB/s of pixels, far more than the
  compressed stream. Strictly worse than decoding locally. This is the same law
  that kills ESP32-side decoding: decoded video is always vastly larger than
  compressed video.
- **Anything with per-frame shared state.**

### 4.4 The honest framing

"A Beowulf cluster of PlayStations" is a genuinely good demo and a terrible
computer. Say so plainly rather than implying it is useful — the joke lands harder
when the numbers are on screen next to it.

---

## 5. Suggested build order

1. **LAN bridge with a built-in delay injector** — two Picos, same network,
   direct UDP, plus a configurable artificial delay on the wire. **These are the
   same box**, not two projects: the bridge already buffers the byte stream, so
   holding bytes for N milliseconds is a parameter, not a feature.

   This one build answers everything. It proves the transport works, and sweeping
   the delay (5 → 20 → 50 → 100 → 200 → 300 ms) produces the playable-distance
   table per game — including whether the ~300 ms France case survives, **without
   needing anyone in France**.

2. **Match server + web lobby** — identity, pairing, role assignment, relay
   fallback.
3. **Long-distance attempt** — the France run.
4. **Clustering demo** — Mandelbrot across two blackroo nodes, if wanted for
   content.

Stage 1 answers every technical question that matters. Stages 2–4 are production
work on top of proven foundations.

### Note on presenting this

Stage 1 is a complete story on its own: two PlayStations playing each other over
WiFi, on screen, working, no caveats. The delay injector then lets the *same rig*
demonstrate the long-distance problem live — dial in 300 ms and show exactly
where it breaks. That turns online play from a promise into a measured result you
can put on screen before writing a single line of server code.

---

## Sources

- `docs/21-PIO-PORT-REFERENCE.md` — PIO port pinout, timing, DMA, I2S
- `docs/13-SIO1-HARDWARE-RESEARCH.md` — SIO1 baud calculation and register layout
- PlayStation Link Cable game list and SCPH-1040 detail —
  <https://en.wikipedia.org/wiki/PlayStation_Link_Cable>
- Latency figures are great-circle distance and fibre propagation estimates, not
  measurements. **Measure the real RTT before relying on them.**
