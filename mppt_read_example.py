#!/usr/bin/python3
from ve_aggregator import VEDirect
from time import sleep, time

PORT = '/dev/ttyUSB0'
BAUD = 19200

with VEDirect(PORT, BAUD) as vd:
	while True:
		sleep(1)
		for pid, d in vd.get_all().items():
			age = round(time() - d.get('ts', 0), 1)
			print(f'{pid}  age={age}s')
			for k, v in d.items():
				if k == 'ts': continue
				print(f'  {k:<12} {v}')
			print()
