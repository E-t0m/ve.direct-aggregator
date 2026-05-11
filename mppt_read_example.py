#!/usr/bin/python3
from ve_aggregator import VEDirect
from time import sleep, time
from datetime import datetime

PORT = '/dev/ttyACM3'
BAUD = 19200
COL  = 24   # column width per device

def header_label(key, d):
	ser = d.get('SER#', '')
	return ser if ser else key

with VEDirect(PORT, BAUD) as vd:
	last_alive_shown = 0.0
	last_ts          = {}   # {key: ts} — print only when all devices updated
	block_count      = {}   # {key: int} — blocks received per device
	start_time       = time()

	while True:
		sleep(0.2)

		if vd._last_alive != last_alive_shown:
			last_alive_shown = vd._last_alive
			ts = datetime.fromtimestamp(last_alive_shown).strftime('%H:%M:%S')
			print(f'MCU last seen: {ts}')

		data = vd.get_all()
		if not data:
			continue

		# count new blocks
		for k, d in data.items():
			if d['ts'] > last_ts.get(k, 0):
				block_count[k] = block_count.get(k, 0) + 1

		now   = time()
		fresh = {k: d['ts'] for k, d in data.items()}
		if all(fresh.get(k, 0) <= last_ts.get(k, 0) for k in fresh):
			continue

		ages = [now - ts for ts in fresh.values()]
		if len(ages) > 1 and (max(ages) - min(ages)) > 2.0:
			continue

		last_ts = dict(fresh)
		pids    = sorted(data.keys())
		runtime = int(now - start_time)

		all_fields = []
		for p in pids:
			for k in data[p]:
				if k not in ('ts', 'PID') and k not in all_fields:
					all_fields.append(k)

		print()
		print(''.join(f'{header_label(p, data[p]):<{COL}}' for p in pids))
		print('-' * COL * len(pids))

		# statistics row
		print(''.join(
			f'{"uptime " + str(runtime) + "s  blocks " + str(block_count.get(p, 0)):<{COL}}'
			for p in pids
		))

		# age row
		print(''.join(
			f'{"age " + str(round(now - data[p].get("ts", 0), 1)) + "s":<{COL}}'
			for p in pids
		))

		for field in all_fields:
			print(''.join(
				f'{field + ": " + str(data[p].get(field, "")):<{COL}}'
				for p in pids
			))
