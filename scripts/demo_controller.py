import socket
import openziti
import json
import time
import select
import sys
import argparse

def list_namespaces(ztx, service, gateway, timeout=5.0):
    """Ask the gateway bridge which robot namespaces it can see on its ROS 2 bus."""
    print(f"[Dials] {gateway}...")
    try:
        conn = ztx.connect(service, gateway)
    except Exception as e:
        print(f"[Error] Could not connect to {gateway}: {e}")
        return 1

    try:
        conn.send((json.dumps({"cmd": "list_ns"}) + "\n").encode())
        buffer = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([conn], [], [], deadline - time.time())
            if not r:
                break
            chunk = conn.recv(4096).decode("utf-8", errors="ignore")
            if not chunk:
                print("[Error] Gateway closed the connection")
                return 1
            buffer += chunk
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                try:
                    data = json.loads(line)
                except ValueError:
                    continue
                if data.get("type") == "ns_list":
                    namespaces = data.get("namespaces", [])
                    print(f"[{gateway}] {len(namespaces)} namespace(s) discovered:")
                    for ns in namespaces:
                        print(f"  {ns}")
                    return 0
        print(f"[Error] No reply from {gateway} within {timeout:.0f}s "
              "(bridge may be an older build without list_ns support)")
        return 1
    finally:
        conn.close()


def main():
    parser = argparse.ArgumentParser(description="Ziti Robotics Mission Control")
    parser.add_argument("--id_json", required=True)
    parser.add_argument("--primary_gw", required=True)
    parser.add_argument("--ns", help="robot namespace to drive (required unless --list_ns)")
    parser.add_argument("--service", required=True)
    parser.add_argument("--backup_gw", default=None)
    parser.add_argument("--list_ns", action="store_true",
                        help="list robot namespaces discovered by the gateway and exit")
    args = parser.parse_args()

    if not args.list_ns and not args.ns:
        parser.error("--ns is required unless --list_ns is given")

    ztx, _ = openziti.load(args.id_json)

    if args.list_ns:
        sys.exit(list_namespaces(ztx, args.service, args.primary_gw))

    GW_LIST = [args.primary_gw]
    if args.backup_gw:
        GW_LIST.append(args.backup_gw)

    current_gw_idx = 0
    conn = None
    buffer = ""

    while True:
        try:
            if conn is None:
                target_gw = GW_LIST[current_gw_idx]
                print(f"\n[Dials] {target_gw}...")
                try:
                    conn = ztx.connect(args.service, target_gw)
                    conn.settimeout(0.01)
                    print(f"[System] Connected")
                except Exception:
                    current_gw_idx = (current_gw_idx + 1) % len(GW_LIST)
                    time.sleep(1.0)
                    continue

            # COMMAND
            cmd = {"ns": args.ns, "topic": "cmd_vel", "lx": 0.2, "az": 0.1}
            conn.send((json.dumps(cmd) + "\n").encode())

            # TELEMETRY - Non-blocking read with select
            r, _, _ = select.select([conn], [], [], 0.05)
            if r:
                chunk = conn.recv(4096).decode('utf-8', errors='ignore')
                if not chunk: raise ConnectionResetError()
                
                buffer += chunk
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    try:
                        data = json.loads(line)
                        js = data.get("joint_states", {})
                        rad = js.get("position")
                        
                        if rad is not None:
                            rad = float(rad)
                            # --- NEW CALCULATIONS ---
                            deg = rad * 57.2958          # Radians to Degrees
                            rotations = rad / 6.28318    # Radians to Full Turns
                            
                            gw_tag = f"[{GW_LIST[current_gw_idx]}]"
                            
                            # Gateway | Namespace | Radians | Degrees | Rotations
                            print(f"\r{gw_tag} {args.ns} | {rad:>7.2f} rad | {deg:>8.1f}° | {rotations:>5.1f} Turns    ", end="", flush=True)
                    except Exception:
                        pass

            time.sleep(0.05)

        except (socket.error, ConnectionError):
            print(f"\n[FAILOVER] Lost {GW_LIST[current_gw_idx]}")
            if conn: conn.close()
            conn = None
            current_gw_idx = (current_gw_idx + 1) % len(GW_LIST)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nExit")
