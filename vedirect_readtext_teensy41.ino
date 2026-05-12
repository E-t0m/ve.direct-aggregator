// VE.Direct readtext — v1.6 — Teensy 4.1
// Serial1..7 RX → devices, Serial8 or SerialUSB → output
// Circular TX queue — no block dropped due to send backpressure.
//
// OUTPUT_USB 0 → TX8 pin (Serial8)
// OUTPUT_USB 1 → SerialUSB native USB (/dev/ttyACM0)

// Teensy 4.1: 512KB RAM — large buffers
#define SERIAL_RX_BUFFER_SIZE 1024  // ~427ms at 19200 baud per port

#define OUTPUT_USB     0
#define ALIVE_TIMEOUT  10000UL
#define BAUD_VEDIRECT  19200
#define BAUD_UPSTREAM  115200
#define BAUD_OUT       19200
#define BUF_SIZE       300          // max bytes per VE.Direct block
#define N              7            // number of input ports
#define Q_SIZE         12           // TX queue depth

#if OUTPUT_USB
	#define SEROUT SerialUSB
#else
	#define SEROUT Serial8
#endif

HardwareSerial* ports[N] = {
	&Serial1, &Serial2, &Serial3, &Serial4,
	&Serial5, &Serial6, &Serial7
};

uint32_t port_baud[N] = {
	BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_VEDIRECT,
	BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_VEDIRECT
};

// ── receive buffers ────────────────────────────────────────────────────
char rx_buf[N][BUF_SIZE];
int  rx_len[N]  = {0, 0, 0, 0, 0, 0, 0};
int  rx_line[N] = {0, 0, 0, 0, 0, 0, 0};

// ── circular TX queue ─────────────────────────────────────────────────
char q_buf[Q_SIZE][BUF_SIZE];
int  q_len[Q_SIZE] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
int  q_head = 0;
int  q_tail = 0;

inline bool q_full()  { return ((q_tail + 1) % Q_SIZE) == q_head; }
inline bool q_empty() { return q_head == q_tail; }

bool q_push(const char* data, int len) {
	if (q_full()) return false;
	memcpy(q_buf[q_tail], data, len);
	q_len[q_tail] = len;
	q_tail = (q_tail + 1) % Q_SIZE;
	return true;
}

// ── output state ──────────────────────────────────────────────────────
int           out_pos   = 0;
unsigned long last_send = 0;

void setup() {
	SEROUT.begin(BAUD_OUT);
	for (int i = 0; i < N; i++) ports[i]->begin(port_baud[i]);
}

// ── receive ────────────────────────────────────────────────────────────

void read_device(int idx) {
	while (ports[idx]->available()) {
		char c = ports[idx]->read();
		if (rx_len[idx] == 0 && (c == '\r' || c == '\n')) continue;
		if (rx_len[idx] >= BUF_SIZE - 1) {
			rx_len[idx]  = 0;
			rx_line[idx] = 0;
			continue;
		}
		rx_buf[idx][rx_len[idx]++] = c;
		if (c == '\n') {
			if (strncmp(&rx_buf[idx][rx_line[idx]], "Checksum\t", 9) == 0) {
				q_push(rx_buf[idx], rx_len[idx]);
				rx_len[idx]  = 0;
				rx_line[idx] = 0;
				return;
			}
			rx_line[idx] = rx_len[idx];
		}
	}
}

// ── send ──────────────────────────────────────────────────────────────

void send_next() {
	if (q_empty()) return;
	while (out_pos < q_len[q_head]) {
		if (SEROUT.availableForWrite() == 0) return;
		SEROUT.write(q_buf[q_head][out_pos++]);
	}
	last_send = millis();
	out_pos   = 0;
	q_head    = (q_head + 1) % Q_SIZE;
}

void send_alive() {
	if (!q_empty()) return;
	if (millis() - last_send >= ALIVE_TIMEOUT) {
		SEROUT.print("ALIVE\r\n");
		last_send = millis();
	}
}

void loop() {
	send_next();
	for (int i = 0; i < N; i++) read_device(i);
	send_next();
	send_alive();
}
