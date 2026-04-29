// VE.Direct readtext — Arduino Mega 2560
// Serial1 RX → device 1
// Serial2 RX → device 2
// Serial3 RX → device 3
// Serial0 TX → output
//
// Output is a plain sequential VE.Direct text stream.
// Each block starts with PID\t... and ends with \r\n\r\n.
// Blocks are sent immediately when complete, one at a time.
// The receiving end identifies devices by their PID field.
//
// Compatible with: any VE.Direct text parser
// Not compatible with: Cerbo GX / Venus GX as direct receiver
//
// Note: Serial0 TX (pin 1) and the USB port share the same chip.
//   For direct USB connection to host: simply plug in USB cable.
//   host sees /dev/ttyUSB0 or /dev/ttyACM0
//   BAUD_OUT must match on both sides.

#define BAUD      19200
#define BAUD_OUT  19200   // set to 115200 for higher throughput
#define BUF_SIZE  512
#define NUM_PORTS 3

HardwareSerial* ports[] = {&Serial1, &Serial2, &Serial3};
const int N = sizeof(ports) / sizeof(ports[0]);

char  buf[NUM_PORTS][BUF_SIZE];
int   buf_len[NUM_PORTS] = {0, 0, 0};
bool  ready[NUM_PORTS]   = {false, false, false};
char  prev[NUM_PORTS]    = {0, 0, 0};

void setup() {
	Serial.begin(BAUD_OUT);
	for (int i = 0; i < N; i++) {
		ports[i]->begin(BAUD);
	}
}

void read_device(int idx) {
	HardwareSerial* port = ports[idx];
	while (port->available() && !ready[idx]) {
		char c = port->read();
		if (c == '\n' && prev[idx] == '\n') {
			if (buf_len[idx] > 0) {
				if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
				ready[idx] = true;
			}
			prev[idx] = 0;
			return;
		}
		prev[idx] = c;
		if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
	}
}

// send one ready block — first ready port wins, no block mixing possible
void send_next() {
	for (int i = 0; i < N; i++) {
		if (ready[i]) {
			Serial.write(buf[i], buf_len[i]);
			Serial.flush();
			buf_len[i] = 0;
			ready[i]   = false;
			prev[i]    = 0;
			return;
		}
	}
}

void loop() {
	for (int i = 0; i < N; i++) {
		read_device(i);
	}
	send_next();
}
