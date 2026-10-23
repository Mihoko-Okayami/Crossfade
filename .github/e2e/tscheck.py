#!/usr/bin/env python3
import json
import sys

H264, AAC = 0x1b, 0x0f

def stamp(b):
	return ((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | b[4] >> 1

def section(p, at):
	s = p[at + 1 + p[at]:]
	return s[:3 + ((s[1] & 0x0f) << 8 | s[2]) - 4]

def program_map(s):
	return next(((s[i + 2] & 0x1f) << 8 | s[i + 3] for i in range(8, len(s) - 3, 4) if s[i] or s[i + 1]), None)

def tracks(s):
	found, i = {}, 12 + ((s[10] & 0x0f) << 8 | s[11])
	while i + 5 <= len(s):
		found[s[i]] = (s[i + 1] & 0x1f) << 8 | s[i + 2]
		i += 5 + ((s[i + 3] & 0x0f) << 8 | s[i + 4])
	return found.get(H264), found.get(AAC)

def frames(es):
	n = i = 0
	while i + 7 <= len(es) and es[i] == 0xff and es[i + 1] & 0xf6 == 0xf0:
		size = (es[i + 3] & 3) << 11 | es[i + 4] << 3 | es[i + 5] >> 5
		if size < 7 or i + size > len(es):
			break
		n, i = n + 1, i + size
	return n

def main(path):
	data = open(path, "rb").read()
	count = len(data) // 188
	report = {"packets": count, "tail": len(data) % 188, "first": [], "gaps": {}, "dropped": 0, "harmful": 0, "fillers": 0, "backwards": 0, "video_gap_ms": 0, "cuts": 0, "seams": 0, "seconds": 0, "sync": True}
	last, pts, sound, es = {}, {}, None, bytearray()
	pmt = video = audio = None
	for i in range(count):
		p = data[i * 188:(i + 1) * 188]
		if p[0] != 0x47:
			report["sync"] = False
			break
		pid = ((p[1] & 0x1f) << 8) | p[2]
		if len(report["first"]) < 3:
			report["first"].append(pid)
		control, cc, start = (p[3] >> 4) & 3, p[3] & 15, bool(p[1] & 0x40)
		at = 5 + p[4] if control & 2 else 4
		if start and control & 1 and pid == 0:
			pmt = program_map(section(p, at))
		elif start and control & 1 and pid == pmt:
			video, audio = tracks(section(p, at))
		if not control & 1 or pid not in (video, audio):
			continue
		size = 188 - at
		report["fillers"] += control == 3 and p[4] == 182 and p[5] == 0 and not start and p[187] == 0 and p[6:187] == b"\xff" * 181
		repeat = pid in last and cc == last[pid]
		if repeat:
			report["dropped"] += 1
			report["harmful"] += start or size > 1
		elif pid in last and cc != (last[pid] + 1) & 15:
			report["gaps"][hex(pid)] = report["gaps"].get(hex(pid), 0) + 1
		last[pid] = cc
		pes = start and at + 14 <= 188 and p[at:at + 3] == b"\x00\x00\x01" and p[at + 7] & 0x80
		if pes:
			t = stamp(p[at + 9:at + 14])
			if pid in pts:
				step = (t - pts[pid]) % (1 << 33)
				if step >= 1 << 32:
					report["backwards"] += 1
				elif pid == video:
					report["video_gap_ms"] = max(report["video_gap_ms"], step // 90)
					report["cuts"] += step >= 9000
					report["seconds"] += step / 90000
			pts[pid] = t
		if pid == audio and not repeat and pes:
			if sound is not None:
				seam = (t - sound - frames(es) * 1920 + (1 << 32)) % (1 << 33) - (1 << 32)
				report["seams"] += seam < 0 or 0 < seam < 4500
			sound, es = t, bytearray(p[at + 9 + p[at + 8]:])
		elif pid == audio and not repeat and sound is not None:
			es += p[at:]
	report["first"] = [hex(x) for x in report["first"]]
	print(json.dumps(report))

if __name__ == "__main__":
	main(sys.argv[1])
