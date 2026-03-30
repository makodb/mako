import socket
import time
import sys

# Configuration
HOST = '127.0.0.1'
PORT = 16380
KEY = b"bench_counter"
NUM_OPS = 10000

def create_connection():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    return s

def send_command(sock, cmd_parts):
    # Construct RESP array
    cmd = f"*{len(cmd_parts)}\r\n".encode()
    for part in cmd_parts:
        if isinstance(part, str):
            part = part.encode()
        elif isinstance(part, int):
            part = str(part).encode()
        cmd += f"${len(part)}\r\n".encode() + part + b"\r\n"
    sock.sendall(cmd)

def read_response(sock):
    f = sock.makefile('rb')
    line = f.readline()
    if not line:
        raise ConnectionError("Server closed connection")
    
    prefix = line[:1]
    payload = line[1:].strip()
    
    if prefix == b'+':
        return payload  # Simple string
    elif prefix == b'$':
        # Bulk string
        length = int(payload)
        if length == -1:
            return None
        data = f.read(length)
        f.read(2) # CRLF
        return data
    elif prefix == b'-':
        raise RuntimeError(f"Redis Error: {payload.decode(errors='replace')}")
    else:
        return payload

def run_benchmark():
    print(f"Connecting to {HOST}:{PORT}...")
    try:
        sock = create_connection()
    except ConnectionRefusedError:
        print(f"Error: Could not connect to {HOST}:{PORT}. Is the server running?")
        return

    print(f"Running Read-Modify-Write Benchmark ({NUM_OPS} ops)...")

    # 1. Initialize
    print("Initializing counter to 0...")
    try:
        send_command(sock, ["SET", KEY, "0"])
        resp = read_response(sock)
        if resp != b'OK':
            print(f"Init failed: {resp}")
            return
    except Exception as e:
        print(f"Init failed: {e}")
        return

    # 2. RMW Loop
    success_count = 0
    start_time = time.time()
    
    # We reuse the socket, but if it fails we might need to reconnect (omitted for simplicity of 'simple' benchmark)
    # If socket fails, the benchmark stops.
    try:
        for i in range(NUM_OPS):
            # GET
            send_command(sock, ["GET", KEY])
            val_bytes = read_response(sock)
            if val_bytes is None:
                current_val = 0
            else:
                current_val = int(val_bytes)
            
            # Increment
            new_val = current_val + 1
            
            # SET
            send_command(sock, ["SET", KEY, new_val])
            resp = read_response(sock)
            
            if resp == b'OK':
                success_count += 1
            else:
                print(f"Op {i} failed: {resp}")

    except Exception as e:
        print(f"\nLoop interrupted/failed at op {i}: {e}")
    finally:
        sock.close()

    end_time = time.time()
    duration = end_time - start_time
    if duration == 0: duration = 0.0001
    
    # Each op is technically 2 commands (GET + SET)
    throughput = NUM_OPS / duration
    
    print(f"\nCompleted operations.")
    print(f"Attempted RMW cycles: {i+1 if 'i' in locals() else 0}")
    print(f"Successful increments: {success_count}")
    print(f"Time taken: {duration:.2f} seconds")
    print(f"Throughput (RMW/sec): {throughput:,.2f}")

    # 3. Verify
    print("\nVerifying final state...")
    try:
        sock = create_connection()
        send_command(sock, ["GET", KEY])
        final_val_bytes = read_response(sock)
        final_val = int(final_val_bytes) if final_val_bytes else 0
        
        print(f"Expected: {success_count}")
        print(f"Actual:   {final_val}")
        
        if final_val == success_count:
            print("Verifier: PASS")
        else:
            print("Verifier: FAIL")
            
        sock.close()
    except Exception as e:
        print(f"Verification check failed: {e}")

if __name__ == "__main__":
    run_benchmark()
