import socket
import threading
import subprocess
import time
import os

SERVER = "../build/practice/epoll_echo_server"

HOST = "127.0.0.1"
PORT = 1024

CLIENTS = 50
MESSAGES = 20

server_proc = None


def start_server(mode):
    global server_proc

    print()
    print("================================================")
    print(f"START TEST [{mode}]")
    print("================================================")

    server_proc = subprocess.Popen(
        [SERVER, mode],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    time.sleep(1)

    if server_proc.poll() is not None:
        print("[FAIL] server start failed")

        stdout, stderr = server_proc.communicate()

        print(stdout)
        print(stderr)

        return False

    print(f"[PASS] server started pid={server_proc.pid}")

    return True


def stop_server():
    global server_proc

    if server_proc:
        server_proc.kill()
        server_proc.wait()

        print("[PASS] server stopped")


def single_client_test():
    s = socket.socket()

    s.connect((HOST, PORT))

    msg = b"hello_epoll"

    s.sendall(msg)

    data = s.recv(4096)

    s.close()

    return data == msg


def worker(cid, results):
    try:
        s = socket.socket()

        s.connect((HOST, PORT))

        for i in range(MESSAGES):
            msg = f"client_{cid}_{i}".encode()

            s.sendall(msg)

            data = s.recv(4096)

            if data != msg:
                results.append(False)
                s.close()
                return

        s.close()

        results.append(True)

    except Exception:
        results.append(False)


def multi_client_test():
    threads = []

    results = []

    for i in range(CLIENTS):
        t = threading.Thread(target=worker, args=(i, results))
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    return all(results)


def large_data_test():
    s = socket.socket()

    s.connect((HOST, PORT))

    data = b"A" * 100000

    s.sendall(data)

    recv = b""

    while len(recv) < len(data):
        chunk = s.recv(4096)

        if not chunk:
            break

        recv += chunk

    s.close()

    return recv == data


def half_packet_test():
    s = socket.socket()

    s.connect((HOST, PORT))

    s.sendall(b"hello_")

    time.sleep(1)

    s.sendall(b"epoll")

    data = s.recv(4096)

    s.close()

    return data == b"hello_epoll"


def disconnect_test():
    s = socket.socket()

    s.connect((HOST, PORT))

    s.sendall(b"disconnect_test")

    s.close()

    time.sleep(1)

    return True


def fd_leak_test():
    pid = server_proc.pid

    fd_count = len(os.listdir(f"/proc/{pid}/fd"))

    print(f"fd count = {fd_count}")

    return fd_count <= 5


def tcp_state_test():
    result = subprocess.check_output(
        ["ss", "-tanp"]
    ).decode()

    lines = [
        line for line in result.splitlines()
        if f":{PORT}" in line
    ]

    close_wait = 0

    for line in lines:
        if "CLOSE-WAIT" in line:
            close_wait += 1

    return close_wait == 0


def et_special_test():
    """
    ET核心测试:
    必须 read until EAGAIN
    """

    s = socket.socket()

    s.connect((HOST, PORT))

    data = b"B" * 200000

    s.sendall(data)

    recv = b""

    while len(recv) < len(data):
        chunk = s.recv(4096)

        if not chunk:
            break

        recv += chunk

    s.close()

    return len(recv) == len(data)


def run_test(name, fn):
    print(f"[TEST] {name:<30}", end="")

    try:
        ok = fn()

        if ok:
            print("[PASS]")
        else:
            print("[FAIL]")

        return ok

    except Exception as e:
        print(f"[EXCEPTION] {e}")

        return False


def test_mode(mode):
    if not start_server(mode):
        return False

    all_ok = True

    try:
        print()

        all_ok &= run_test(
            "single client test",
            single_client_test
        )

        all_ok &= run_test(
            "multi client test",
            multi_client_test
        )

        all_ok &= run_test(
            "large data test",
            large_data_test
        )

        all_ok &= run_test(
            "half packet test",
            half_packet_test
        )

        all_ok &= run_test(
            "disconnect test",
            disconnect_test
        )

        all_ok &= run_test(
            "fd leak test",
            fd_leak_test
        )

        all_ok &= run_test(
            "tcp state test",
            tcp_state_test
        )

        if mode == "ET":
            all_ok &= run_test(
                "ET read-until-EAGAIN test",
                et_special_test
            )

        print()

        if all_ok:
            print(f"[ALL PASS] {mode}")
        else:
            print(f"[SOME TEST FAILED] {mode}")

        return all_ok

    finally:
        stop_server()


def main():
    lt_ok = test_mode("LT")

    et_ok = test_mode("ET")

    print()
    print("================================================")
    print("FINAL RESULT")
    print("================================================")

    print(f"LT : {'PASS' if lt_ok else 'FAIL'}")
    print(f"ET : {'PASS' if et_ok else 'FAIL'}")


if __name__ == "__main__":
    main()