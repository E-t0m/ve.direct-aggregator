#!/usr/bin/python3
from ve_aggregator import VEDirect
from time import sleep, time
from datetime import datetime

PORT = '/dev/ttyACM3'
BAUD = 19200
COL  = 26

def header_label(key, d):
	ser = d.get('SER#', '')
	return ser if ser else key

with VEDirect(PORT, BAUD) as vd:
	last_alive_shown = 0.0
	last_ts          = {}
	block_count      = {}
	block_times      = {}   # {key: [last N arrival times]}
	start_time       = time()

	while True:
		sleep(0.2)

		if vd._last_alive != last_alive_shown:
			last_alive_shown = vd._last_alive
			# total avg across all devices
			all_intervals = []
			for times in block_times.values():
				if len(times) >= 2:
					diffs = [times[i]-times[i-1] for i in range(1, len(times))]
					all_intervals.extend(diffs)
			avg_str = f'  avg {sum(all_intervals)/len(all_intervals)*1000:.0f}ms' if all_intervals else ''
			ts = datetime.fromtimestamp(last_alive_shown).strftime('%H:%M:%S')
			print(f'MCU last seen: {ts}{avg_str}')

		data = vd.get_all()
		if not data:
			continue

		now = time()
		for k, d in data.items():
			if d['ts'] > last_ts.get(k, 0):
				block_count[k] = block_count.get(k, 0) + 1
				if k not in block_times: block_times[k] = []
				block_times[k].append(d['ts'])
				if len(block_times[k]) > 20: block_times[k].pop(0)

		fresh = {k: d['ts'] for k, d in data.items()}

		if all(fresh.get(k, 0) <= last_ts.get(k, 0) for k in fresh):
			continue

		active_ages = [now - ts for ts in fresh.values() if now - ts < 3.0]
		if len(active_ages) > 1 and (max(active_ages) - min(active_ages)) > 2.0:
			continue

		last_ts = dict(fresh)
		pids    = sorted(data.keys())
		runtime = int(now - start_time)
		ages    = {k: now - ts for k, ts in fresh.items()}

		all_fields = []
		for p in pids:
			for k in data[p]:
				if k not in ('ts', 'PID') and k not in all_fields:
					all_fields.append(k)

		def dev_avg(k):
			times = block_times.get(k, [])
			if len(times) < 2: return ''
			diffs = [times[i]-times[i-1] for i in range(1, len(times))]
			return f' avg {sum(diffs)/len(diffs)*1000:.0f}ms'

		print()
		print(''.join(f'{header_label(p, data[p]):<{COL}}' for p in pids))
		print('-' * COL * len(pids))
		print(''.join(
			f'{f"uptime {runtime:>4}s  blocks {block_count.get(p, 0)}":<{COL}}'
			for p in pids
		))
		print(''.join(
			f'{f"age {round(ages[p]*1000):>4}ms   {dev_avg(p):>4}":<{COL}}'
			for p in pids
		))
		for field in all_fields:
			print(''.join(
				f'{field + ": " + str(data[p].get(field, "")):<{COL}}'
				for p in pids
			))
