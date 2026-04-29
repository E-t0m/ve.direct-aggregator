// VE.Direct readtext — Teensy 4.1
// Serial1–7 RX → device 1–7


// Serial8  TX → output (or SerialUSB if OUTPUT_USB=1)
//
// Output is a plain sequential VE.Direct text stream.
// Each block starts with PID\t... and ends with \r\n\r\n.
// Blocks are sent immediately when complete, one at a time.
// The receiving end identifies devices by their PID field.
//
// Compatible with: any VE.Direct text parser
// Not compatible with: Cerbo GX / Venus GX as direct receiver
//



// Note: VE.Direct is 5V TTL — use BSS138 level shifter on each RX input.
//
// Output selection:
//   OUTPUT_USB 0  →  Serial8 TX8 pin
//   OUTPUT_USB 1  →  SerialUSB native USB (host sees /dev/ttyACM0)
#define OUTPUT_USB 0

#if OUTPUT_USB
  #define SEROUT SerialUSB
#else
  #define SEROUT Serial8
#endif

#define BAUD      19200
#define BAUD_OUT  19200   // set to 115200 for higher throughput
#define BUF_SIZE  512
#define NUM_PORTS 7

HardwareSerial* ports[] = {
	&Serial1, &Serial2, &Serial3, &Serial4,
	&Serial5, &Serial6, &Serial7
};
const int N = sizeof(ports) / sizeof(ports[0]);

char  buf[NUM_PORTS][BUF_SIZE];
int   buf_len[NUM_PORTS] = {0, 0, 0, 0, 0, 0, 0};
bool  ready[NUM_PORTS]   = {false, false, false, false, false, false, false};
char  prev[NUM_PORTS]    = {0, 0, 0, 0, 0, 0, 0};

void setup() {
	#if OUTPUT_USB
	SEROUT.begin(0);
#else
	SEROUT.begin(BAUD_OUT);
#endif;
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
			SEROUT.write(buf[i], buf_len[i]);
			SEROUT.flush();
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
