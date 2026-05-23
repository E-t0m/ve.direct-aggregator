#!/usr/bin/python3
# block_monitor.py — v2.0 — block timing monitor with per-device interval
from ve_aggregator import VEDirect
from time import sleep
from datetime import datetime

PORT = '/dev/ttyACM3'
BAUD = 19200

def fmt(ts):
	return datetime.fromtimestamp(ts).strftime('%H:%M:%S.%f')[:-3]

with VEDirect(PORT, BAUD) as vd:
	last_ts   = {}
	prev_time = {}   # {ser: previous arrival time}

	while True:
		sleep(0.05)
		data = vd.get_all()
		if not data: continue

		for k, d in data.items():
			if d['ts'] == last_ts.get(k): continue
			last_ts[k] = d['ts']
			ser = d.get('SER#', k)
			now = d['ts']

			interval = ''
			if ser in prev_time:
				interval = f'  {(now - prev_time[ser]) * 1000:.0f}ms'
			prev_time[ser] = now

			print(f'{fmt(now)}  {ser}{interval}', flush=True)
