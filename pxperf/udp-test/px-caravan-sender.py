#!/usr/bin/env python3
"""
PX-caravan test sender (scapy).

Builds one *outer* UDP datagram whose payload is a PX-caravan:

    IP(tos=0x01) | outer UDP | iUDPhdr1 | payload1 | iUDPhdr2 | payload2 | ...

and sends it to the receiver.  The kernel patch (udp_recvmsg, ToS 0x01) walks
the inner UDP headers using each inner header's `len` field, so we let scapy
compute every inner UDP `len` (= 8 + payload) for us.  The *outer* UDP checksum
must be correct (the GRO path validates it once), which scapy also computes
because the outer UDP sits on top of a real IP layer.

Run on the SENDER machine as root (raw sockets):

    sudo python3 px-caravan-sender.py --src 10.2.95.2 --dst 10.2.90.2 --lens 1000,1000,1000
    sudo python3 px-caravan-sender.py --src 10.2.95.2 --dst 10.2.90.2 --lens 1000,1000,400   # len1>len2 (short tail)
    sudo python3 px-caravan-sender.py --src 10.2.95.2 --dst 10.2.90.2 --lens 500,1000,1000   # len1<len2 (no aggregation on seg0)
    sudo python3 px-caravan-sender.py --src 10.2.95.2 --dst 10.2.90.2 --count 4 --size 1200  # 4 equal segments

The script prints what a correct kernel should return on the receiver for both
the GRO path (--gro) and the partial-copy path (no --gro), so you can diff the
real receiver output against it.
"""
import argparse
import struct
import sys

try:
    from scapy.all import IP, UDP, Raw, send, conf, fragment
except ImportError:
    sys.stderr.write("scapy not installed: pip3 install scapy\n")
    raise

UDP_HDR = 8                 # sizeof(struct udphdr)
PX_GRO_CNT_MAX = 64         # mirror of PX_GRO_CNT_MAX / UDP_GRO_CNT_MAX


def seg_payload(seg_idx, plen):
    """Marker payload for one segment: 'S<idx>|' then a repeated letter."""
    marker = ("S%d|" % seg_idx).encode()
    if plen <= len(marker):
        return marker[:plen]
    fill = bytes([(seg_idx % 26) + ord('A')]) * (plen - len(marker))
    return marker + fill


def build_inner(seg_idx, sport, dport, plen):
    """One inner segment = inner UDP header (len auto = 8+plen) + marker payload."""
    # Standalone UDP -> scapy fills the `len` field (8+plen). Inner checksum is
    # irrelevant to the kernel (only the outer csum is verified), so leave it 0.
    return bytes(UDP(sport=sport, dport=dport, chksum=0) /
                 Raw(load=seg_payload(seg_idx, plen)))


def kernel_gro_runs(lens):
    """Predict the GRO path: replicate the kernel's run-collection.

    Returns a list of (gso_size, recv_len, nsegs) -- one tuple per recvmsg().
    Termination mirrors udp_recvmsg()'s GRO branch / udp_gro_receive_segment():
      equal length -> coalesce; shorter -> include as final segment, stop;
      longer -> stop before it (new datagram); cap at PX_GRO_CNT_MAX.
    """
    runs = []
    i, n = 0, len(lens)
    while i < n:
        gso = lens[i]
        total, cnt, j = 0, 0, i
        while j < n:
            pl = lens[j]
            if cnt and pl > gso:
                break                 # longer than gso_size -> new datagram
            total += pl
            cnt += 1
            j += 1
            if pl < gso:
                break                 # shorter -> last segment of this run
            if cnt >= PX_GRO_CNT_MAX:
                break
        runs.append((gso, total, cnt))
        i = j
    return runs


def main():
    ap = argparse.ArgumentParser(description="PX-caravan scapy sender")
    ap.add_argument("--dst", required=True, help="receiver IP")
    ap.add_argument("--src", help="source IP (set this! otherwise scapy may emit 0.0.0.0, "
                                  "which the receiver drops as a martian / rp_filter source)")
    ap.add_argument("--dport", type=int, default=5001, help="outer UDP dst port (receiver bind port)")
    ap.add_argument("--mtu", type=int, default=9000, help="path MTU (jumbo by default); larger caravans are IP-fragmented")
    ap.add_argument("--sport", type=int, default=40000, help="outer UDP src port")
    ap.add_argument("--tos", type=lambda x: int(x, 0), default=0x01,
                    help="IP ToS byte that triggers the patch (default 0x01)")
    ap.add_argument("--lens", help="comma-separated inner payload lengths, e.g. 1000,1000,400")
    ap.add_argument("--count", type=int, default=3, help="number of segments (when --lens absent)")
    ap.add_argument("--size", type=int, default=1000, help="per-segment payload (when --lens absent)")
    ap.add_argument("--inner-port", type=int, default=5001, help="inner UDP src/dst port (cosmetic)")
    ap.add_argument("--repeat", type=int, default=1, help="send the caravan N times (throughput)")
    ap.add_argument("--iface", help="outgoing interface (optional)")
    ap.add_argument("--fmt", choices=["hdr", "prefix2b"], default="hdr",
                    help="hdr = inner UDP headers (px_caravan 0/1/2); "
                         "prefix2b = 2-byte length prefix + header-less payloads (px_caravan 3)")
    args = ap.parse_args()

    if args.lens:
        lens = [int(x) for x in args.lens.split(",") if x != ""]
    else:
        lens = [args.size] * args.count
    if not lens:
        ap.error("no segments")

    # Assemble the caravan payload.
    if args.fmt == "prefix2b":
        # mode 3 format: L(2B, network order) | payload1 | ... | payloadN
        # (uniform L; the kernel reads L as gso_size and delivers payloads.)
        if len(set(lens)) != 1:
            print("[build] NOTE: prefix2b assumes uniform L; using lens[0]=%d" % lens[0])
        caravan = struct.pack("!H", lens[0]) + b"".join(
            seg_payload(idx, plen) for idx, plen in enumerate(lens))
    else:
        caravan = b"".join(
            build_inner(idx, args.inner_port, args.inner_port, plen)
            for idx, plen in enumerate(lens))

    ip = IP(dst=args.dst, tos=args.tos)
    if args.src:
        ip.src = args.src
    pkt = ip / UDP(sport=args.sport, dport=args.dport) / Raw(load=caravan)

    # --- report ---
    seg_tots = [UDP_HDR + pl for pl in lens]
    print("[build] %s -> %s:%d  sport=%d  tos=0x%02x"
          % (pkt[IP].src, args.dst, args.dport, args.sport, args.tos))
    if pkt[IP].src in (None, "0.0.0.0"):
        print("[build] WARNING: source IP is 0.0.0.0 -> receiver will drop it (martian). Pass --src <sender IP>.")
    print("[build] inner payload lens : %s" % lens)
    print("[build] inner seg_tot (8+pl): %s" % seg_tots)
    print("[build] outer UDP payload   : %d bytes (= sum seg_tot)" % len(caravan))
    frame_len = len(caravan) + UDP_HDR + 20  # + outer UDP + IP header
    will_frag = frame_len > args.mtu

    runs = kernel_gro_runs(lens)
    print("[expect][--gro] %d recvmsg call(s)  (PX-aware: inner UDP hdrs included):"
          % len(runs))
    for k, (gso, rl, cnt) in enumerate(runs, 1):
        # PX-aware: gso_size = 8 + payload, recv_len includes the 8B hdr per seg
        a_gso = gso + UDP_HDR
        a_rl = rl + cnt * UDP_HDR
        print("          #%d  each_size(gso_size)=%d  recv_len=%d  (%d seg%s, each = 8B hdr + %dB payload)"
              % (k, a_gso, a_rl, cnt, "" if cnt == 1 else "s", gso))
    print("[expect][no --gro] %d recvmsg call(s), one inner payload each, no cmsg:" % len(lens))
    print("          recv_len sequence = %s" % lens)

    if args.iface:
        conf.iface = args.iface

    if will_frag:
        fragsize = (args.mtu - 20) & ~7   # IP payload per fragment, multiple of 8
        out = fragment(pkt, fragsize=fragsize)
        print("[send] frame %dB > MTU %d -> %d IP fragments; %d caravan(s)..."
              % (frame_len, args.mtu, len(out), args.repeat))
    else:
        out = pkt
        print("[send] frame %dB; %d caravan(s)..." % (frame_len, args.repeat))

    send(out, count=args.repeat, verbose=False)
    print("[send] done.")


if __name__ == "__main__":
    main()
