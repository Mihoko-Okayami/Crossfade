#!/usr/bin/env python3
import json
import socket
import sys
import time

def closed_after(sock, limit):
	start = time.monotonic()
	sock.settimeout(limit)
	try:
		while sock.recv(4096):
			pass
	except socket.timeout:
		return None
	except ConnectionError:
		pass
	return round(time.monotonic() - start, 2)

def silence(host, port):
	sock = socket.create_connection((host, port))
	print(json.dumps({"closed_after": closed_after(sock, 20)}))

def request(sock, text, limit):
	sock.sendall(text.encode())
	sock.settimeout(limit)
	start, reply = time.monotonic(), b""
	while b"\r\n\r\n" not in reply:
		chunk = sock.recv(4096)
		if not chunk:
			break
		reply += chunk
	return reply.split(b"\r\n", 1)[0].decode(errors="replace"), round(time.monotonic() - start, 2)

def wait(host, port, stream):
	sock = socket.create_connection((host, port))
	uri = f"rtsp://{host}:{port}/{stream}"
	first, _ = request(sock, f"OPTIONS {uri} RTSP/1.0\r\nCSeq: 1\r\n\r\n", 5)
	time.sleep(3)
	status, seconds = request(sock, f"DESCRIBE {uri} RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n", 15)
	print(json.dumps({"options": first, "describe": status, "seconds": seconds}))

def limit(host, port, count):
	socks, alive, refused = [], 0, 0
	for _ in range(count):
		try:
			socks.append(socket.create_connection((host, port), timeout=5))
		except OSError:
			refused += 1
	time.sleep(1)
	for sock in socks:
		sock.setblocking(False)
		try:
			refused += sock.recv(1) == b""
		except BlockingIOError:
			alive += 1
		except ConnectionError:
			refused += 1
		sock.close()
	print(json.dumps({"alive": alive, "refused": refused}))

def shared(host, port, count, http_port):
	socks = []
	for _ in range(count):
		try:
			socks.append(socket.create_connection((host, port), timeout=5))
		except OSError:
			pass
	time.sleep(0.5)
	statuses = {}
	cases = (
		("same", f"X-Forwarded-For: 203.0.113.1, {host}\r\n"),
		("other", "X-Forwarded-For: 203.0.113.1, 198.51.100.7\r\n"),
		("lines", f"X-Forwarded-For: 198.51.100.8\r\nX-Forwarded-For: {host}\r\n"),
		("long", f"X-Forwarded-For: {'198.51.100.9, ' * 25}{host}\r\n"),
	)
	for name, headers in cases:
		sock = socket.create_connection(("127.0.0.1", http_port), timeout=10)
		sock.sendall(f"GET /vrc.ts HTTP/1.1\r\nHost: probe\r\n{headers}\r\n".encode())
		try:
			statuses[name] = sock.recv(64).split(b"\r\n", 1)[0].decode(errors="replace")
		except OSError as error:
			statuses[name] = str(error)
		sock.close()
	for sock in socks:
		sock.close()
	print(json.dumps(statuses))

if __name__ == "__main__":
	mode, host, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
	if mode == "silence":
		silence(host, port)
	elif mode == "wait":
		wait(host, port, sys.argv[4])
	elif mode == "shared":
		shared(host, port, int(sys.argv[4]), int(sys.argv[5]))
	else:
		limit(host, port, int(sys.argv[4]))
