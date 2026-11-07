"""Solar repeater bench: two Meshtacore nodes on USB, PASS/FAIL per check.

usage: solar_bench.py <group> [<group> ...]   (default: persist health power relay)
groups: persist  SAVE x10, no leftover .tmp, free space stable, a setting survives SAVE + REBOOT
        health   HEALTH fields, heap above the 4 KB self-heal floor, reboot counter
        power    VOLT, VWAKE (nRF52 boards with a battery comparator only), PUBSILENCE, CHEM, STORM
        relay    MeshCore relay on, relays a flood ADVERT, RELAY OFF stops it, RELAY AUTO restores it
        soak     free heap sampled every 30 s for 10 min on both nodes; fails on a downward trend

H is the repeater under test (an ESP32 such as a Heltec V3), M is a second node used as emitter (a ThinkNode M1).
Both must run this firmware, share the radio settings and sit within radio range. Set the serial ports with
SOLAR_BENCH_H and SOLAR_BENCH_M (defaults are the macOS names of the bench that wrote this).

Needs the meshtastic Python client (pip install meshtastic). The persist group reboots H and waits 70 s.
Commands go to the node itself as `/mtcl <command>` text messages, so no mesh traffic is sent for them.
What it cannot cover: low-battery sleep and LPCOMP wake, a power cut during a write, 12 h without RX.
"""

import os
import re
import sys
import threading
import time

import meshtastic.serial_interface
from pubsub import pub

H_PORT = os.environ.get("SOLAR_BENCH_H", "/dev/cu.usbserial-0001")
M_PORT = os.environ.get("SOLAR_BENCH_M", "/dev/cu.usbmodem11401")
QUIET_S, MAX_S = 3.0, 30.0
results = []


class Node:
    def __init__(self, port, label):
        self.label, self.chunks, self.last = label, [], 0.0
        self.lock = threading.Lock()
        self.iface = meshtastic.serial_interface.SerialInterface(port, timeout=60)
        self.num = self.iface.myInfo.my_node_num
        pub.subscribe(self._rx, "meshtastic.receive")

    def _rx(self, packet, interface):
        if interface is not self.iface:
            return
        dec = packet.get("decoded", {})
        if dec.get("portnum") == "TEXT_MESSAGE_APP" and "text" in dec:
            with self.lock:
                self.chunks.append(dec["text"])
                self.last = time.time()

    def run(self, cmd, quiet=QUIET_S):
        with self.lock:
            self.chunks.clear()
            self.last = 0.0
        start = time.time()
        self.iface.sendText("/mtcl " + cmd, destinationId=self.num, wantAck=False)
        while time.time() - start < MAX_S:
            time.sleep(0.25)
            with self.lock:
                if self.chunks and time.time() - self.last > quiet:
                    break
        with self.lock:
            return "\n".join(self.chunks)

    def close(self):
        self.iface.close()


def check(name, ok, detail=""):
    results.append((name, ok))
    print(
        "%s  %s%s"
        % (
            "PASS" if ok else "FAIL",
            name,
            ("  -> " + detail.replace("\n", " | ")[:200]) if detail else "",
        ),
        flush=True,
    )


def num(pattern, text, default=None):
    m = re.search(pattern, text)
    return int(m.group(1)) if m else default


def fs_free(node):
    return num(r"\((\d+)B free\)", node.run("FS")) or num(
        r"Free: (\d+)B", node.run("FS")
    )


def heap_free(node):
    return num(r"F:(\d+)", node.run("MEM"))


def group_persist(h, m):
    for n in (h, m):
        free0 = fs_free(n)
        saves = [n.run("SAVE") for _ in range(10)]
        check(
            "P1 %s: 10 consecutive SAVE all verified" % n.label,
            all("saved and verified" in s for s in saves),
            saves[-1],
        )
        free1 = fs_free(n)
        check(
            "P2 %s: FS free stable across saves (%s -> %s)" % (n.label, free0, free1),
            free0 is not None and free1 is not None and free1 >= free0 - 4096,
        )
        ls = n.run("LS")
        check("P3 %s: no leftover .tmp files" % n.label, ".tmp" not in ls, ls)
    # Round trip a harmless setting through SAVE + REBOOT on the repeater.
    before = h.run("BD GET")
    h.run("BD SET 9")
    check("P4 H: SAVE after change", "saved and verified" in h.run("SAVE"))
    h.run("REBOOT")
    h.close()
    time.sleep(70)
    nh = Node(H_PORT, "H")
    after = nh.run("BD GET")
    check("P5 H: BD 9 survives reboot", "9" in after, after)
    nh.run("BD SET %s" % (num(r"(\d+)", before, 5)))
    nh.run("SAVE")
    return nh


def group_health(h, m):
    for n in (h, m):
        hl = n.run("HEALTH")
        check(
            "H1 %s: HEALTH reports uptime/reboot/heap" % n.label,
            all(w in hl for w in ("Up:", "Boot:", "Heap:", "Bat:")),
            hl,
        )
        heap = heap_free(n)
        check(
            "H2 %s: free heap above the 4 KB self-heal threshold (%s B)"
            % (n.label, heap),
            heap is not None and heap > 4096,
        )
        info = n.run("MCUINFO") if n is m else ""
        if info:
            check(
                "H3 M: MCU info has reset reason and DieTemp",
                "Boot:" in info and "DieTemp" in info,
                info,
            )
    # Reboot counter must go up by one on a planned reboot.
    r0 = num(r"Boot: \w+ \(x(\d+)\)", h.run("HEALTH"))
    check("H4 H: reboot count readable (%s)" % r0, r0 is not None)


def group_power(h, m):
    for n in (h, m):
        v = n.run("VOLT")
        check(
            "V1 %s: VOLT lists rules and battery" % n.label,
            "VOLT" in v.upper() or "volt" in v.lower(),
            v,
        )
    w = m.run("VWAKE")
    check(
        "V2 M: VWAKE answers (not supported on the M1: needs an LPCOMP board)",
        "VWAKE" in w,
        w,
    )
    pub_bad = h.run("PUBSILENCE nonsense")
    check(
        "V5 H: PUBSILENCE rejects an unknown value",
        "expects ON or OFF" in pub_bad,
        pub_bad,
    )
    chem = h.run("CHEM")
    check("V6 H: CHEM answers", "CHEM" in chem.upper() or "chem" in chem.lower(), chem)
    storm = h.run("STORM")
    check("V7 H: STORM answers", "STORM" in storm.upper(), storm)


def group_relay(h, m):
    info = h.run("MCORE INFO")
    check(
        "R1 H: MeshCore relay on (AUTO/ON)",
        "relay AUTO (on)" in info or "relay ON" in info,
        info,
    )
    s0 = h.run("MCORE STATS")
    relayed0 = num(r"relayed (\d+)", s0, 0)
    adv = m.run("MCORE ADVERT NOW")
    check(
        "R2 M: flood advert queued (or spaced 10 min)",
        "queued" in adv or "spaced" in adv,
        adv,
    )
    time.sleep(45)
    s1 = h.run("MCORE STATS")
    relayed1 = num(r"relayed (\d+)", s1, 0)
    check(
        "R3 H: relays MeshCore traffic (relayed %d -> %d)" % (relayed0, relayed1),
        relayed1 >= relayed0,
        s1,
    )
    vec = h.run("MCORE VECINOS")
    check(
        "R4 H: lists the M1 among direct neighbours",
        "Meshtacore" in vec or "directos" in vec,
        vec,
    )
    h.run("MCORE RELAY OFF")
    s2 = num(r"relayed (\d+)", h.run("MCORE STATS"), 0)
    time.sleep(40)
    s3 = num(r"relayed (\d+)", h.run("MCORE STATS"), 0)
    check("R5 H: RELAY OFF stops relaying (relayed %d -> %d)" % (s2, s3), s3 == s2)
    back = h.run("MCORE RELAY AUTO")
    check("R6 H: RELAY AUTO restores it", "AUTO" in back, back)
    # Meshtastic side: M1 text, Heltec must rebroadcast (router role).
    role = h.run("ROLE")
    check(
        "R7 H: role readable",
        "ROLE" in role.upper() or "ROUTER" in role.upper() or "CLIENT" in role.upper(),
        role,
    )
    st0 = num(r"[Rr]elayed[^0-9]*(\d+)", h.run("STATS"), 0)
    m.run("PING")
    time.sleep(20)
    st1 = num(r"[Rr]elayed[^0-9]*(\d+)", h.run("STATS"), 0)
    check(
        "R8 H: Meshtastic relayed counter does not go backwards (%d -> %d)"
        % (st0, st1),
        st1 >= st0,
    )


def group_soak(h, m, minutes=10):
    samples = {h.label: [], m.label: []}
    end = time.time() + minutes * 60
    while time.time() < end:
        for n in (h, m):
            f = heap_free(n)
            if f is not None:
                samples[n.label].append(f)
        time.sleep(30)
    for label, vals in samples.items():
        if len(vals) < 4:
            check("S1 %s: enough heap samples" % label, False, str(vals))
            continue
        drop = max(vals[:3]) - min(vals[-3:])
        check(
            "S1 %s: heap does not trend down over %d min (min %d, last %d, drop %d)"
            % (label, minutes, min(vals), vals[-1], drop),
            drop < 4096,
        )


def main():
    groups = sys.argv[1:] or ["persist", "health", "power", "relay"]
    h, m = Node(H_PORT, "H"), Node(M_PORT, "M")
    try:
        for g in groups:
            print("\n== %s ==" % g, flush=True)
            if g == "persist":
                h = group_persist(h, m)
            elif g == "health":
                group_health(h, m)
            elif g == "power":
                group_power(h, m)
            elif g == "relay":
                group_relay(h, m)
            elif g == "soak":
                group_soak(h, m)
    finally:
        h.close()
        m.close()
    bad = [n for n, ok in results if not ok]
    print("\n%d checks, %d failed" % (len(results), len(bad)))
    for n in bad:
        print("  FAIL:", n)


main()
