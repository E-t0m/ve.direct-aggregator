// VE.Direct Aggregator — Teensy 4.1
// Serial1–7 RX ← charger 1–7
// Serial8   TX → output
// Baud: 19200 on all inputs
//
// Marker before each packet:
//   ---\tN\r\n  (N = number of blocks)
// A downstream aggregator uses this for transparent pass-through.
//
// Note: VE.Direct is 5V TTL — use BSS138 level shifter on each RX input.
//       Teensy 4.1 operates at 3.3V logic.

#define BAUD      19200
#define BAUD_OUT  19200   // safe to increase to 115200 on Teensy 4.1
#define BUF_SIZE  512

HardwareSerial* ports[] = {
	&Serial1, &Serial2, &Serial3, &Serial4,
	&Serial5, &Serial6, &Serial7
};
const int N = sizeof(ports) / sizeof(ports[0]);

char  buf[7][BUF_SIZE];
int   buf_len[7] = {0, 0, 0, 0, 0, 0, 0};
bool  ready[7]   = {false, false, false, false, false, false, false};
char  prev[7]    = {0, 0, 0, 0, 0, 0, 0};

void setup() {
	Serial8.begin(BAUD_OUT);   // output
	for (int i = 0; i < N; i++) {
		ports[i]->begin(BAUD);
	}
}

void read_charger(int idx) {
	HardwareSerial* port = ports[idx];
	while (port->available() && !ready[idx]) {
		char c = port->read();
		if (c == '\n' && prev[idx] == '\n') {
			// block end: two consecutive \n
			if (buf_len[idx] > 0) {
				if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
				ready[idx] = true;
			}
			prev[idx] = 0;   // always reset — prevents false trigger on next block
			return;
		}
		prev[idx] = c;
		if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
	}
}

void send_blocks() {
	int count = 0;
	for (int i = 0; i < N; i++) {
		if (ready[i]) count++;
	}
	if (count == 0) return;

	// send marker: ---\tN\r\n
	Serial8.print("---\t");
	Serial8.print(count);
	Serial8.print("\r\n");

	// send ready blocks sequentially
	for (int i = 0; i < N; i++) {
		if (ready[i]) {
			Serial8.write(buf[i], buf_len[i]);
			Serial8.flush();
			buf_len[i] = 0;
			ready[i]   = false;
			prev[i]    = 0;
		}
	}
}

void loop() {
	for (int i = 0; i < N; i++) {
		read_charger(i);
	}
	send_blocks();
}
