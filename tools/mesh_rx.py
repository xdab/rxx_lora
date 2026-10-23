#!/usr/bin/env python3
"""Run rtl_lora and pretty-print the Meshtastic packets it receives.

References: meshtastic.org/docs/overview/mesh-algo (on-air header layout),
meshtastic.org/docs/development/reference/encryption-technical (AES-CTR
nonce). The decode helpers mirror mesh_udp.py; its UDP backend is replaced
by rtl_lora.
"""

import argparse
import base64
import json
import os
import re
import struct
import subprocess
import sys
import threading
import time

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from google.protobuf import text_format
from google.protobuf.message import DecodeError
from meshtastic.protobuf import (
    admin_pb2,
    atak_pb2,
    mesh_pb2,
    mqtt_pb2,
    paxcount_pb2,
    portnums_pb2,
    remote_hardware_pb2,
    storeforward_pb2,
    telemetry_pb2,
)
from meshtastic.util import DEFAULT_KEY, message_to_json

P = portnums_pb2.PortNum
NODES = {}  # node DB: "!aabbccdd" -> {short_name, position, packets, ...}

# portnum -> protobuf message for the app payload (None = raw text)
APP = {
    P.POSITION_APP: mesh_pb2.Position,
    P.NODEINFO_APP: mesh_pb2.User,
    P.ROUTING_APP: mesh_pb2.Routing,
    P.ADMIN_APP: admin_pb2.AdminMessage,
    P.TEXT_MESSAGE_COMPRESSED_APP: mesh_pb2.Compressed,
    P.WAYPOINT_APP: mesh_pb2.Waypoint,
    P.TELEMETRY_APP: telemetry_pb2.Telemetry,
    P.TRACEROUTE_APP: mesh_pb2.RouteDiscovery,
    P.NEIGHBORINFO_APP: mesh_pb2.NeighborInfo,
    P.PAXCOUNTER_APP: paxcount_pb2.Paxcount,
    P.REMOTE_HARDWARE_APP: remote_hardware_pb2.HardwareMessage,
    P.STORE_FORWARD_APP: storeforward_pb2.StoreAndForward,
    P.ATAK_PLUGIN: atak_pb2.TAKPacket,
    P.ATAK_PLUGIN_V2: atak_pb2.TAKPacketV2,
    P.MAP_REPORT_APP: mqtt_pb2.MapReport,
    P.NODE_STATUS_APP: mesh_pb2.StatusMessage,
    P.KEY_VERIFICATION_APP: mesh_pb2.KeyVerification,
}
TEXT_APP = {P.TEXT_MESSAGE_APP, P.RANGE_TEST_APP}

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = re.compile(r"^(?:TRACE|DEBUG|INFO|WARN|ERROR): ")


def psk_key(psk):
    """Expand a channel PSK into an AES key (1-byte PSKs index the default key)."""
    return DEFAULT_KEY[:-1] + psk if len(psk) == 1 else psk


def aes_ctr(key, nonce, data):
    enc = Cipher(algorithms.AES(key), modes.CTR(nonce)).encryptor()
    return enc.update(data) + enc.finalize()


def nonce(packet_id, from_node):
    return struct.pack("<QI", packet_id, from_node) + b"\0" * 4


def load_keys(path, extra):
    """Candidate (label, key) pairs: default key, any --map channel, any --key."""
    keys, seen = [("default", DEFAULT_KEY)], {DEFAULT_KEY}
    if path and os.path.exists(path):
        with open(path) as f:
            for ch in json.load(f):
                psk = base64.b64decode(ch["psk"])
                key = psk_key(psk)
                if key not in seen:
                    seen.add(key)
                    keys.append((f"map[{ch.get('name', '')}]", key))
    for spec in extra:
        key = psk_key(base64.b64decode(spec))
        if key not in seen:
            seen.add(key)
            keys.append((f"key={spec}", key))
    return keys


def load_db(path):
    """Read the node DB, or return an empty one if it is missing/corrupt."""
    try:
        with open(path) as f:
            db = json.load(f)
    except (OSError, ValueError):
        return {}
    return db if isinstance(db, dict) else {}


def save_db(path, db):
    """Atomically write the node DB (whole-file replace, no partial writes)."""
    tmp = f"{path}.tmp"
    with open(tmp, "w") as f:
        json.dump(db, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


def packet_from_air(raw):
    """16-byte on-air header + ciphertext -> MeshPacket, or None if too short."""
    if len(raw) < 16:
        return None
    to, frm, pid = struct.unpack_from("<III", raw, 0)
    flags = raw[12]
    pkt = mesh_pb2.MeshPacket()
    setattr(pkt, "from", frm)
    pkt.to = to
    pkt.id = pid
    pkt.hop_limit = flags & 0x7
    pkt.want_ack = (flags >> 3) & 1
    pkt.via_mqtt = (flags >> 4) & 1
    pkt.hop_start = (flags >> 5) & 0x7
    pkt.channel = raw[13]  # channel hash, the mesh_udp map key convention
    pkt.next_hop = raw[14]
    pkt.relay_node = raw[15]
    pkt.encrypted = raw[16:]
    return pkt


def try_data(key, pkt):
    """Decrypt pkt.encrypted with key; return the Data, or None on garbage."""
    data = mesh_pb2.Data()
    try:
        data.ParseFromString(aes_ctr(key, nonce(pkt.id, getattr(pkt, "from")), pkt.encrypted))
    except DecodeError:
        return None
    return data


def decrypt(pkt, keys):
    """Try each key; on a Data whose source matches the header, attach it."""
    for label, key in keys:
        data = try_data(key, pkt) if pkt.encrypted else None
        if data is not None and data.source in (0, getattr(pkt, "from")):
            pkt.decoded.CopyFrom(data)  # wrong key -> garbage source, filtered
            return label
    return None


def app_text(data):
    """Full text form of the app payload, or '' when there is nothing typed."""
    if data.portnum in TEXT_APP:
        return data.payload.decode("utf-8", "replace")
    msg = app_message(data)
    if msg is None:
        return f"<undecodable {APP[data.portnum].DESCRIPTOR.name}>"
    return text_format.MessageToString(msg, as_utf8=False).rstrip()


def app_message(data):
    """Typed app payload message, or None for text/unknown/undecodable."""
    cls = APP.get(data.portnum)
    if cls is None:
        return None
    msg = cls()
    try:
        msg.ParseFromString(data.payload)
    except DecodeError:
        return None
    return msg


def ename(msg, field):
    """Name of an enum field's value, or its number when unrecognized."""
    fd = msg.DESCRIPTOR.fields_by_name[field]
    value = fd.enum_type.values_by_number.get(getattr(msg, field))
    return value.name if value else str(getattr(msg, field))


def summarize(data):
    """One-line human summary of the app payload ('' when there is none)."""
    if data.portnum in TEXT_APP:
        return data.payload.decode("utf-8", "replace")
    if data.portnum == P.POSITION_APP:
        pos = mesh_pb2.Position()
        pos.ParseFromString(data.payload)
        text = f"{pos.latitude_i * 1e-7:.5f}, {pos.longitude_i * 1e-7:.5f}"
        if pos.altitude:
            text += f"  alt {pos.altitude} m"
        return f"{text}  ({ename(pos, 'location_source')})"
    if data.portnum == P.NODEINFO_APP:
        user = mesh_pb2.User()
        user.ParseFromString(data.payload)
        return (
            f'{user.id} "{user.long_name}" ({user.short_name})  '
            f"{ename(user, 'hw_model')}  {ename(user, 'role')}"
        )
    msg = app_message(data)
    if msg is None:
        return ""
    return "  ".join(text_format.MessageToString(msg, as_utf8=False).split())


def portname(data):
    try:
        return P.Name(data.portnum)
    except ValueError:
        return f"UNKNOWN({data.portnum})"


def node(num):
    tag = "*" if num == 0xFFFFFFFF else f"!{num:08x}"
    name = NODES.get(tag, {}).get("short_name")
    return f"{tag} ({name})" if name else tag


def print_packet(pkt, meta, key_label, as_json, verbose):
    """Pretty-print one reconstructed MeshPacket."""
    if as_json:
        print(message_to_json(pkt), flush=True)  # one JSON object per line
        return
    frm = getattr(pkt, "from")
    hops = pkt.hop_start - pkt.hop_limit
    decoded = pkt.WhichOneof("payload_variant") == "decoded"
    name = portname(pkt.decoded) if decoded else "ENCRYPTED"
    flag = f"{meta.get('snr', '?')} dB  crc {meta.get('crc', '?')}"
    key = f"  key {key_label}" if verbose and decoded else ""
    print(flush=True)
    print(f"[{time.strftime('%H:%M:%S')}] {name:<22} {flag}{key}")
    print(
        f"  from {node(frm)}  to {node(pkt.to)}  hop {pkt.hop_limit}/{pkt.hop_start}"
        f" ({hops} away)  id 0x{pkt.id:08x}  chan 0x{pkt.channel:02x}  cfo {meta.get('cfo', '?')} Hz"
    )
    if not decoded:
        print(f"  no channel key matched ({len(pkt.encrypted)} B)")
        return
    if verbose:
        for line in app_text(pkt.decoded).splitlines():
            print(f"  {line}")
    else:
        text = summarize(pkt.decoded)
        if text:
            print(f"  {text}")
    sys.stdout.flush()


def record_meta(rec):
    """One JSONL frame record -> the meta dict print_packet/learn expect."""
    if not rec["crc_present"]:
        crc = "absent"
    elif rec["crc_valid"]:
        crc = "valid!"
    else:
        crc = "invalid"
    return {"b64": rec["packet"], "snr": rec["snr_db"], "cfo": rec["cfo_hz"], "crc": crc}


def feed(args, keys):
    """Yield one meta dict per decoded frame from rtl_lora's JSONL stdout.

    rtl_lora writes one JSON object per decoded frame to stdout; its
    human-readable progress stays on stderr and is forwarded only for
    WARN/ERROR (or in full with --raw). --replay reads a saved stdout
    capture (a JSONL file), not the old combined log.
    """
    if args.replay:
        with open(args.replay) as f:
            for line in f:
                if line.strip():
                    yield record_meta(json.loads(line))
        return

    cmd = [args.bin] + args.rtl_args
    print(f"# {' '.join(cmd)}", flush=True)
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1
    )

    def drain_stderr():
        for line in proc.stderr:
            line = line.rstrip("\n")
            level = LOG.match(line)
            level = level.group(0).rstrip(": ") if level else ""
            if args.raw or level in ("WARN", "ERROR"):
                print(LOG.sub("", line), flush=True)

    reader = threading.Thread(target=drain_stderr, daemon=True)
    reader.start()
    try:
        for line in proc.stdout:
            if line.strip():
                yield record_meta(json.loads(line))
    finally:
        proc.terminate()
        proc.wait()


def learn(nodes, pkt, meta, b64):
    """Update the node DB from one packet: identity, position, telemetry, raw."""
    tag = f"!{getattr(pkt, 'from'):08x}"
    now = time.strftime("%Y-%m-%dT%H:%M:%S")
    rec = nodes.setdefault(tag, {"id": tag, "first_heard": now, "packets": []})
    rec["last_heard"] = now
    if meta.get("snr"):
        rec["snr"] = float(meta["snr"])
    if pkt.channel:
        rec["channel"] = pkt.channel
    if b64 not in rec["packets"]:  # keep the raw on-air payloads this node sent
        rec["packets"].append(b64)
    if pkt.WhichOneof("payload_variant") != "decoded":
        return
    data = pkt.decoded
    if data.portnum == P.NODEINFO_APP:
        user = mesh_pb2.User()
        user.ParseFromString(data.payload)
        rec["long_name"] = user.long_name
        rec["short_name"] = user.short_name
        rec["hw_model"] = ename(user, "hw_model")
        rec["role"] = ename(user, "role")
        if user.public_key:
            rec["public_key"] = base64.b64encode(user.public_key).decode()
        if user.macaddr:
            rec["macaddr"] = user.macaddr.hex()
    elif data.portnum == P.POSITION_APP:
        pos = mesh_pb2.Position()
        pos.ParseFromString(data.payload)
        rec["position"] = {
            "lat": round(pos.latitude_i * 1e-7, 7),
            "lon": round(pos.longitude_i * 1e-7, 7),
            "alt": pos.altitude,
            "time": pos.time,
            "source": ename(pos, "location_source"),
        }
    elif data.portnum == P.TELEMETRY_APP:
        tel = telemetry_pb2.Telemetry()
        tel.ParseFromString(data.payload)
        if tel.HasField("device_metrics"):
            dm = tel.device_metrics
            rec["telemetry"] = {
                f: getattr(dm, f) for f in ("battery_level", "voltage", "uptime_seconds")
            }


def emit(pending, keys, as_json, verbose):
    """Decode one buffered RX frame, update the node DB, and print it."""
    b64 = pending["b64"]
    raw = base64.b64decode(b64)
    pkt = packet_from_air(raw)
    if pkt is None:
        print(f"short payload ({len(raw)} B), skipped", flush=True)
        return
    if pending.get("snr"):
        pkt.rx_snr = float(pending["snr"])
    label = decrypt(pkt, keys)
    learn(NODES, pkt, pending, b64)
    print_packet(pkt, pending, label, as_json, verbose)


def main():
    epilog = """\
Any argument that is not one of the tool options above is passed through
verbatim to rtl_lora (`--` also works as an explicit separator). rtl_lora's
human-readable INFO chatter stays on stderr and is hidden by default; use
--raw to see it.

rtl_lora emits one JSON object per decoded frame on stdout (`packet` is the
base64 on-air payload, alongside snr, cfo and the CRC verdict). A Meshtastic
payload is a 16-byte on-air header + AES-CTR encrypted Data:

  offset 0  uint32 le  destination node (!ffffffff = broadcast)
  offset 4  uint32 le  sender node
  offset 8  uint32 le  packet id
  offset 12 uint8      flags: hop_limit(3) want_ack(1) via_mqtt(1) hop_start(3)
  offset 13 uint8      channel hash
  offset 14 uint8      next-hop low byte
  offset 15 uint8      relay-node low byte
  offset 16 ...        ciphertext

This script rebuilds the MeshPacket struct, decrypts with a channel map
(nonce = LE64(id) || LE32(from) || 4 zero bytes), then prints the decoded
portnum protobuf.

It also keeps a persistent node DB (node-db.json in the cwd, --no-db to skip):
each sender's names, hardware, role, public key, last position/telemetry,
last-heard time and every raw base64 payload it has been seen sending.

examples:
  %(prog)s --center-freq 869.525e6 -s9 -w 0x2b --bw 250e3 -o4
  %(prog)s --replay capture.jsonl --json
"""
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0]
        + " Options not listed below are forwarded to rtl_lora.",
        usage="%(prog)s [tool options] [--] <rtl_lora options...>",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=epilog,
    )
    ap.add_argument(
        "--bin",
        default=os.path.join(ROOT, "bin", "rtl_lora"),
        help="rtl_lora binary (default: %(default)s)",
    )
    ap.add_argument(
        "--map",
        default=os.path.join(ROOT, "channels.json"),
        help="channel map file from mesh_udp.py (default: %(default)s)",
    )
    ap.add_argument(
        "--key",
        action="append",
        default=[],
        metavar="B64",
        help="extra channel PSK, base64 (repeatable)",
    )
    ap.add_argument(
        "--replay",
        metavar="FILE",
        help="read a saved rtl_lora stdout capture (JSONL) instead of running the radio",
    )
    ap.add_argument(
        "--db",
        default="node-db.json",
        metavar="FILE",
        help="persistent node database in $(pwd) (default: %(default)s)",
    )
    ap.add_argument("--no-db", action="store_true", help="do not load or save the node database")
    ap.add_argument("--json", action="store_true", help="print each MeshPacket as protobuf JSON")
    ap.add_argument(
        "--verbose",
        action="store_true",
        help="dump the full app-level protobuf instead of a one-line summary",
    )
    ap.add_argument(
        "--raw",
        action="store_true",
        help="echo every rtl_lora log line, not just decoded packets (WRN/ERR show by default)",
    )
    args, args.rtl_args = ap.parse_known_args()
    if not args.replay and not args.rtl_args:
        ap.error(
            "pass rtl_lora options (e.g. --center-freq 869.525e6 -s9 -w 0x2b) or --replay FILE"
        )

    keys = load_keys(args.map, args.key)
    if not args.no_db:
        NODES.update(load_db(args.db))
    try:
        for pending in feed(args, keys):
            try:
                emit(pending, keys, args.json, args.verbose)
            except BrokenPipeError:
                raise
            except Exception as e:  # noqa: BLE001 - a malformed frame must not stop the run
                print(f"skipping malformed packet: {type(e).__name__}: {e}", flush=True)
            if not args.no_db:
                save_db(args.db, NODES)
    finally:
        if not args.no_db and NODES:
            save_db(args.db, NODES)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
    except BrokenPipeError:  # a downstream reader (e.g. `| head`) closed early
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        sys.exit(0)
