// VE.Direct readtext -- v2.0 -- Arduino Mega 2560
//
// Reads N VE.Direct text streams (Serial1..3 RX) and multiplexes them
// into one aggregated stream on Serial0 TX (19200 baud).
//
// Complete blocks are forwarded immediately; blocks are dropped silently
// if the TX queue is full (receiver will notice missing Checksum lines).
//
// HEX frames injected by devices into the text stream are stripped
// transparently ? at line start, mid-line, and after the Checksum byte.
//
// Responds to RESET\n by propagating downstream then resetting MCU.
// Responds to WHO\n with READTEXT Mega2560 N=<N>\n
// Sends ALIVE\r\n every 10s when idle.
//
// Optional: DS18B20 1-Wire temperature sensor(s) on TEMP_PIN.
// Each sensor emits a pseudo VE.Direct block (PID=0x9999, SER#=TEMP-PN-SN).
// No sensor connected -> no blocks, no overhead.
//
// Output consumed by ve_aggregator.py and/or vedirect_deaggregator.py.
// vedirect_deaggregator.py creates one virtual serial port per device
// for Venus OS / Cerbo GX integration.

#define SERIAL_RX_BUFFER_SIZE 256   // ~107ms at 19200 baud per port

#define ALIVE_TIMEOUT  10000UL
#define BAUD_VEDIRECT  19200
#define BAUD_UPSTREAM  115200
#define BAUD_OUT       19200
#define BUF_SIZE       256          // max bytes per VE.Direct block
#define N              3            // number of input ports
#define Q_SIZE         12           // TX queue depth

// DS18B20 1-Wire temperature sensor
// Set TEMP_ENABLE to 0 to disable. Set TEMP_PIN to the DATA pin.
// Wiring: VCC->5V, GND->GND, DATA->TEMP_PIN, 4.7k pull-up 5V->DATA.
// Requires: OneWire + DallasTemperature (Arduino Library Manager).
#define TEMP_ENABLE     1
#define TEMP_PIN        2
#define TEMP_INTERVAL   5000UL

#if TEMP_ENABLE
#include <avr/wdt.h>
#include <OneWire.h>
#include <DallasTemperature.h>
OneWire            temp_wire(TEMP_PIN);
DallasTemperature  temp_sensors(&temp_wire);
unsigned long      temp_last  = 0;
int                temp_count = 0;
#endif
#if BUF_SIZE > SERIAL_RX_BUFFER_SIZE
#error "BUF_SIZE exceeds SERIAL_RX_BUFFER_SIZE ? increase hardware RX buffer"
#endif

HardwareSerial* ports[N] = {&Serial1, &Serial2, &Serial3};
uint32_t port_baud[N]    = {BAUD_VEDIRECT, BAUD_VEDIRECT, BAUD_VEDIRECT};

// -- receive buffers (one per port) ------------------------------------
char rx_buf[N][BUF_SIZE];
int  rx_len[N]  = {0, 0, 0};

// -- circular TX queue -------------------------------------------------
char q_buf[Q_SIZE][BUF_SIZE];
int  q_len[Q_SIZE] = {0};
int  q_head = 0;   // next slot to send
int  q_tail = 0;   // next free slot

inline bool q_full()  { return ((q_tail + 1) % Q_SIZE) == q_head; }
inline bool q_empty() { return q_head == q_tail; }

bool q_push(const char* data, int len) {
	if (q_full() || len > BUF_SIZE) return false;
	memcpy(q_buf[q_tail], data, len);
	q_len[q_tail] = len;
	q_tail = (q_tail + 1) % Q_SIZE;
	return true;
}

// -- output state ------------------------------------------------------
int           out_pos   = 0;
unsigned long last_send = 0;


#if TEMP_ENABLE
void send_temp_blocks() {
	if (temp_count == 0) return;
	if (millis() - temp_last < TEMP_INTERVAL) return;
	temp_last = millis();
	for (int s = 0; s < temp_count; s++) {
		float t = temp_sensors.getTempCByIndex(s);
		if (t == DEVICE_DISCONNECTED_C) continue;
		char ser[24], tmp[16];
		sprintf(ser, "TEMP-P%d-S%d", TEMP_PIN, s);
		sprintf(tmp, "%.2f", t);
		char blk[256];
		int  pos = 0;
		pos += sprintf(blk + pos, "PID\t0x9999\r\n");
		pos += sprintf(blk + pos, "SER#\t%s\r\n", ser);
		pos += sprintf(blk + pos, "FW\t100\r\n");
		pos += sprintf(blk + pos, "TEMP\t%s\r\n", tmp);
		uint8_t sum = 0;
		for (int i = 0; i < pos; i++) sum += (uint8_t)blk[i];
		const char* cs_label = "Checksum\t";
		for (int i = 0; i < 9; i++) sum += (uint8_t)cs_label[i];
		sum += '\r'; sum += '\n';
		uint8_t cs_byte = (256 - sum) & 0xFF;
		pos += sprintf(blk + pos, "Checksum\t");
		blk[pos++] = (char)cs_byte;
		blk[pos++] = '\r';
		blk[pos++] = '\n';
		blk[pos]   = '\0';
		q_push(blk, pos);
	}
	temp_sensors.requestTemperatures();
}
#endif

void setup() {
	Serial.begin(BAUD_OUT);
#if TEMP_ENABLE
	temp_sensors.begin();
	temp_count = temp_sensors.getDeviceCount();
	temp_sensors.setResolution(12);
	temp_sensors.setWaitForConversion(false);
	temp_sensors.requestTemperatures();
	temp_last = millis();
#endif
	for (int i = 0; i < N; i++) ports[i]->begin(port_baud[i]);
}

// -- receive ------------------------------------------------------------

// forward declaration
void reset_rx(int idx);

static int line_start(const char* buf, int len) {
	for (int i = len - 2; i >= 0; i--)
		if (buf[i] == '\n') return i + 1;
	return 0;
}

void reset_rx(int idx) { rx_len[idx] = 0; }

bool hex_skip[N] = {false, false, false};

void read_device(int idx) {
	while (ports[idx]->available()) {
		char c = ports[idx]->read();
		// skip remainder of a HEX frame split across loop() calls
		if (hex_skip[idx]) {
			if (c == '\n') {
				hex_skip[idx] = (ports[idx]->available() && ports[idx]->peek() == ':');
			}
			continue;
		}
		if (rx_len[idx] == 0 && (c == '\r' || c == '\n')) continue;
		if (rx_len[idx] == 0 && c == ':') { hex_skip[idx] = true; continue; }
		if (rx_len[idx] >= BUF_SIZE - 1) { reset_rx(idx); continue; }
		rx_buf[idx][rx_len[idx]++] = c;
		if (c == '\n') {
			int ls = line_start(rx_buf[idx], rx_len[idx]);
			if (rx_buf[idx][ls] == ':') { rx_len[idx] = ls; continue; }
			// detect HEX frame injected mid-line (e.g. "HSDS\t0:4AAAAFD\n")
			for (int j = ls; j < rx_len[idx] - 1; j++) {
				if (rx_buf[idx][j] == ':' && (j == 0 || rx_buf[idx][j-1] != 'x')) {
					rx_len[idx] = j; hex_skip[idx] = true; break;
				}
			}
			if (hex_skip[idx]) continue;
			if (strncmp(&rx_buf[idx][ls], "Checksum\t", 9) == 0) {
				int end = ls + 10;
				while (end < rx_len[idx] && rx_buf[idx][end] != ':' && rx_buf[idx][end] != '\n') end++;
				if (end < rx_len[idx] && rx_buf[idx][end] == '\n') end++;
				q_push(rx_buf[idx], end);   // drop silently if full ? receiver will notice
				if (end < rx_len[idx] && rx_buf[idx][end] == ':') hex_skip[idx] = true;
				reset_rx(idx);
				continue;
			}
		}
	}
}

// -- send --------------------------------------------------------------

void send_next() {
	if (q_empty()) return;

	// send bytes from current head block
	while (out_pos < q_len[q_head]) {
		if (Serial.availableForWrite() == 0) return;
		Serial.write(q_buf[q_head][out_pos++]);
	}

	// block fully sent
	last_send = millis();
	out_pos   = 0;
	q_head    = (q_head + 1) % Q_SIZE;
}

void send_alive() {
	if (!q_empty()) return;
	if (millis() - last_send >= ALIVE_TIMEOUT) {
		Serial.print("ALIVE\r\n");
		last_send = millis();
	}
}


void forward_all(const char* msg) {
	for (int i = 0; i < N; i++) {
		ports[i]->print(msg);
		ports[i]->print('\n');
	}
}

void read_cmd() {
	static char buf[8];
	static int  len = 0;
	while (Serial.available()) {
		char c = Serial.read();
		if (c == '\n' || c == '\r') {
			buf[len] = '\0';
			if (strcmp(buf, "RESET") == 0) {
				forward_all("RESET");
				delay(50);
				wdt_enable(WDTO_15MS);
				while(1);
			}
			if (strcmp(buf, "WHO") == 0)
				Serial.print("READTEXT Mega2560 N=" + String(N) + "\n");
			len = 0;
		} else if (len < 7) {
			buf[len++] = c;
		}
	}
}

void loop() {
	read_cmd();
	send_next();
	for (int i = 0; i < N; i++) read_device(i);
	send_next();
#if TEMP_ENABLE
	send_temp_blocks();
	send_next();
#endif
	send_alive();
}
