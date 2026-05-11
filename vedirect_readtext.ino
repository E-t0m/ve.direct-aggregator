// VE.Direct readtext — v1.2 — Arduino Mega 2560
// Serial1/2/3 RX → devices, Serial0 TX → output (also via USB)
// Block end: Checksum\t line. Devices identified by PID + SER#.
// Not compatible with Cerbo GX / Venus GX as direct receiver.

#define ALIVE_TIMEOUT  10000UL  // ms without block → send ALIVE\r\n
#define BAUD_VEDIRECT  19200    // VE.Direct standard — fixed by Victron
#define BAUD_UPSTREAM  115200   // MCU-to-MCU in cascade topology
#define BAUD_OUT       19200    // output — set to BAUD_UPSTREAM for cascade
#define BUF_SIZE       512      // max bytes per block (~200 typical)
#define N              3        // number of input ports

HardwareSerial* ports[N] = {&Serial1, &Serial2, &Serial3};

// per-port input baud — change to BAUD_UPSTREAM for cascade inputs
uint32_t port_baud[N] = {BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_VEDIRECT};

char buf[N][BUF_SIZE];
int  buf_len[N]    = {0, 0, 0};
int  line_start[N] = {0, 0, 0};
bool ready[N]      = {false, false, false};

char          send_buf[BUF_SIZE];
int           send_port = -1;
int           send_pos  =  0;
int           send_len  =  0;
unsigned long last_send =  0;

void setup() {
	Serial.begin(BAUD_OUT);
	for (int i = 0; i < N; i++) ports[i]->begin(port_baud[i]);
}

void read_device(int idx) {
	HardwareSerial* port = ports[idx];
	while (port->available() && !ready[idx]) {
		char c = port->read();
		if (buf_len[idx] == 0 && (c == '\r' || c == '\n')) continue;
		if (buf_len[idx] < BUF_SIZE - 1) buf[idx][buf_len[idx]++] = c;
		if (c == '\n') {
			if (strncmp(&buf[idx][line_start[idx]], "Checksum\t", 9) == 0) {
				ready[idx] = true;
				return;
			}
			line_start[idx] = buf_len[idx];
		}
	}
}

void send_next() {
	if (send_port >= 0) {
		while (send_pos < send_len) {
			if (Serial.availableForWrite() == 0) return;
			Serial.write(send_buf[send_pos++]);
		}
		last_send = millis();
		send_port = -1;
		return;
	}
	for (int i = 0; i < N; i++) {
		if (ready[i]) {
			memcpy(send_buf, buf[i], buf_len[i]);
			send_len      = buf_len[i];
			send_port     = i;
			send_pos      = 0;
			buf_len[i]    = 0;
			line_start[i] = 0;
			ready[i]      = false;
			return;
		}
	}
}

void send_alive() {
	if (millis() - last_send >= ALIVE_TIMEOUT) {
		Serial.print("ALIVE\r\n");
		last_send = millis();
	}
}

void loop() {
	send_next();
	for (int i = 0; i < N; i++) read_device(i);
	send_next();
	send_alive();
}
