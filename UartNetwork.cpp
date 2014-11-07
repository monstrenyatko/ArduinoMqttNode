/* Internal Includes */
#include "UartNetwork.h"
#include "Lpm.h"
#include "GoliathMqttSensor.h"
/* External Includes */
/* System Includes */

UartNetwork::UartNetwork(const UartNetworkConfig& config)
:mConfig(config)
{
	Serial.begin(mConfig.speed);
	while (!Serial);
}

int UartNetwork::connect(const char* hostname, int port) {
	return 0;
}

int UartNetwork::connect(uint32_t hostname, int port) {
	return 0;
}

int UartNetwork::read(unsigned char* buffer, int len, int timeoutMs) {
	Serial.setTimeout(timeoutMs);
	const int intervalMs = (timeoutMs < mConfig.readIdlePeriodLongMs)
			? mConfig.readIdlePeriodShortMs : mConfig.readIdlePeriodLongMs;
	int totalMs = 0;
	int readLen = 0;
	while(readLen < len && totalMs < timeoutMs) {
		int available = Serial.available();
		if (available) {
			int needToRead = len-readLen;
			int qty = Serial.readBytes((char*) buffer, needToRead);
			if (qty<0) {
				// error => return with current read len
				break;
			}
			buffer+=qty;
			readLen+=qty;
		} else {
			Lpm::idle(intervalMs);
			totalMs += intervalMs;
		}
	}
	return readLen;
}

int UartNetwork::write(unsigned char* buffer, int len, int timeoutMs) {
	Serial.setTimeout(timeoutMs);
	for (int i = 0; i < len; ++i) {
		Serial.write(buffer[i]);
	}
	Serial.flush();
	return len;
}

int UartNetwork::disconnect() {
	return 0;
}