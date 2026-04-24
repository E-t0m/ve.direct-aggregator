// VE.Direct Aggregator — Arduino Mega 2560
// Serial1 RX → charger 1
// Serial2 RX → charger 2
// Serial3 RX → charger 3
// Serial0 TX → output
// Baud: 19200 on all inputs
//
// Marker before each packet:
//   ---\tN\r\n  (N = number of blocks)
// A downstream aggregator uses this for transparent pass-through.

#define BAUD      19200
#define BAUD_OUT  19200   // increase to 115200 for higher throughput
#define BUF_SIZE  512

HardwareSerial* ports[] = {&Serial1, &Serial2, &Serial3};
const int N = sizeof(ports) / sizeof(ports[0]);

char  buf[3][BUF_SIZE];
int   buf_len[3] = {0, 0, 0};
bool  ready[3]   = {false, false, false};
char  prev[3]    = {0, 0, 0};

void setup() {
	Serial.begin(BAUD_OUT);
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
			prev[idx] = 0;   // reset always — prevents false trigger on next block
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
	Serial.print("---\t");
	Serial.print(count);
	Serial.print("\r\n");

	// send ready blocks sequentially
	for (int i = 0; i < N; i++) {
		if (ready[i]) {
			Serial.write(buf[i], buf_len[i]);
			Serial.flush();
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
