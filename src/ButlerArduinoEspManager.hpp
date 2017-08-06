/*
 *******************************************************************************
 *
 * Purpose: ESP manager implementation.
 *
 *******************************************************************************
 * Copyright Oleg Kovalenko 2017.
 *
 * Distributed under the MIT License.
 * (See accompanying file LICENSE or copy at http://opensource.org/licenses/MIT)
 *******************************************************************************
 */

#ifndef BUTLER_ARDUINO_ESP_MANAGER_H_
#define BUTLER_ARDUINO_ESP_MANAGER_H_

/* System Includes */
#include <Arduino.h>
#include <stdint.h>
#include <WString.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
/* Internal Includes */
#include "ButlerArduinoStrings.hpp"
#include "ButlerArduinoLogger.hpp"
#include "ButlerArduinoContext.hpp"
#include "ButlerArduinoEspStorage.hpp"
#include "ButlerArduinoEspLpm.hpp"
#include "ButlerArduinoEspTime.hpp"
#include "ButlerArduinoUtil.hpp"
#include "ButlerArduinoHwUart.hpp"
#include "ButlerArduinoEspWiFiConfigCaptivePortal.hpp"
#include "ButlerArduinoEspHttpUpdate.hpp"
#include "ButlerArduinoArrayBuffer.hpp"


namespace Butler {
namespace Arduino {

struct RotateFingerprintsStatus {
	typedef enum {
		OK,
		ERROR,
		ERROR_VERIFY
	} Type;
};

struct AuthenticateStatus {
	typedef enum {
		OK,
		ERROR,
		ERROR_FORBIDDEN
	} Type;
};

template<class CONFIG_T>
class EspManager {
public:
	EspManager(uint32_t* lpmData = nullptr, uint32_t lpmDataSize = 0)
		: mLpmData(lpmData), mLpmDataSize(lpmDataSize),
		mHwUart({getConfig().HW_UART_SPEED}), mHttpUpdate(getContext())
	{}

	//// ACTIONS ////
	/** Configures all modules. Must be called ASAP on board start. */
	void setup() {
		//// ID ////
		mId = Util::macAddressToHex(WiFi.macAddress());
		//// NAME ////
		mName = String(getConfig().NAME_PREFIX) + String(ESP.getChipId(), HEX);
		//// TIME ////
		mCtx.time = &getClock();
		//// LOG ////
		mCtx.logger = &getHwUart();
		LOG_PRINTFLN(getContext(), "%s", Strings::EMPTY);
		//// LPM ////
		mCtx.lpm = &mLpm;
		//// CONFIGURATION ////
		bool configLoaded = getConfig().load(getContext(), getConfigStorage());
		//// SETUP MODE
		if (configLoaded) {
			setupNormalMode();
		} else {
			setupConfigMode();
		}
	}

	/** Restarts board. */
	void restart() {
		yield();
		ESP.reset();
	}

	/** Prints board current state like: heap, time, etc... */
	void printState() {
		LOG_PRINTFLN(getContext(), "#################################");
		LOG_PRINTFLN(getContext(), "###      State");
		LOG_PRINTFLN(getContext(), "### Heap Free   : %lu B", ESP.getFreeHeap());
		LOG_PRINTFLN(getContext(), "### Time        : %lu Ms", getClock().millis());
		LOG_PRINTFLN(getContext(), "#################################");
	}

	/** Puts board to sleep. */
	void idle(uint32_t ms, bool debug = true) {
		if (debug) {
			LOG_PRINTFLN(getContext(), "Sleep for %lu Ms", ms);
		}
		getLpm().idle(ms, mLpmData, mLpmDataSize);
	}

	/** Waits the Network/WiFi connection. */
	bool waitNetwork(bool sleepOnFailure = true) {
		{
			LOG_PRINTFLN(getContext(), "[manager] Waiting the WiFi");
			Time::Timer timer(getClock(), getConfig().NET_CONNECT_TM_MS);
			while (!timer.expired() && WiFi.status() != WL_CONNECTED) {
				LOG_PRINTFLN(getContext(), ".");
				delay(500);
			}
		}
		bool connected = (WiFi.status() == WL_CONNECTED);
		if (connected) {
			LOG_PRINTFLN(getContext(), "[manager] Connected to WiFi");
			LOG_PRINTFLN(getContext(), "[manager] IP: %s", WiFi.localIP().toString().c_str());
		} else {
			LOG_PRINTFLN(getContext(), "[manager] ERROR, WiFi is not connected");
			if (sleepOnFailure) {
				uint32_t retryTm = getConfig().NET_CONNECT_ERROR_RETRY_TM_MS;
				LOG_PRINTFLN(getContext(), "Retry in %lu Ms", retryTm);
				idle(retryTm, false);
			}
		}
		return connected;
	}

	bool connectServer(Client &client, const String &host, uint16_t port, bool sleepOnFailure = true) {
		LOG_PRINTFLN(getContext(), "[manager] Connecting to port: %u", port);
		client.connect(host.c_str(), port);
		bool connected = client.connected();
		if (connected) {
			LOG_PRINTFLN(getContext(), "[manager] Connected to Server");
		} else {
			LOG_PRINTFLN(getContext(), "[manager] ERROR, Server is not connected");
			if (sleepOnFailure) {
				uint32_t retryTm = getConfig().NET_CONNECT_ERROR_RETRY_TM_MS;
				LOG_PRINTFLN(getContext(), "Retry in %lu Ms", retryTm);
				idle(retryTm, false);
			}
		}
		return connected;
	}

	/** Waits the time data using NTP protocol. */
	bool waitNtpTime(const char* ntpServer = nullptr, bool sleepOnFailure = true) {
		if (!ntpServer) {
			ntpServer = getConfig().SERVER_ADDR;
		}
		getClock().initRtc(ntpServer);
		bool updated = (0 != getClock().rtc());
		if (updated) {
			LOG_PRINTFLN(getContext(), "[manager] NTP time: %lu", getClock().rtc());
		} else {
			LOG_PRINTFLN(getContext(), "[manager] ERROR, NTP time is not available");
			if (sleepOnFailure) {
				uint32_t retryTm = getConfig().NET_CONNECT_ERROR_RETRY_TM_MS;
				LOG_PRINTFLN(getContext(), "Retry in %lu Ms", retryTm);
				idle(retryTm, false);
			}
		}
		return updated;
	}

	/** Checks and installs the FW update. */
	HTTPUpdateResult checkFirmwareUpdate() {
		String url = Util::makeUrl(Strings::URL_MODEL_UPDATE_FW,
				getConfig().SERVER_ADDR, getConfig().SERVER_HTTPS_PORT
		);
		return getHttpUpdate().update(url, getConfig().auth.fingerprints[0], getConfig().auth.token);
	}

	/** Checks and installs the FW update using HTTP. */
	HTTPUpdateResult checkFirmwareUpdateNotSecure() {
		String url = Util::makeUrl(Strings::URL_MODEL_UPDATE_FW_NOT_S,
				getConfig().SERVER_ADDR, getConfig().SERVER_HTTP_PORT
		);
		Util::setModelKey(url, Strings::MODEL_KEY_ID, getId());
		return getHttpUpdate().update(url);
	}

	/** Checks and installs the server fingerprints. */
	bool checkServerFingerprintsUpdate(bool secure = true) {
		bool res = false;
		String payload;
		{
			HTTPClient http;
			if (secure) {
				http.begin(Util::makeUrl(
						Strings::URL_MODEL_FINGERPRINTS,
						getConfig().SERVER_ADDR,
						getConfig().SERVER_HTTPS_PORT
					),
					getConfig().auth.fingerprints[0]
				);
			} else {
				LOG_PRINTFLN(getContext(), "[manager] WARN, Fingerprints Update via HTTP");
				http.begin(Util::makeUrl(
						Strings::URL_MODEL_FINGERPRINTS_NOT_S,
						getConfig().SERVER_ADDR,
						getConfig().SERVER_HTTP_PORT
				));
			}
			int httpCode = http.GET();
			if (httpCode > 0) {
				LOG_PRINTFLN(getContext(), "[manager] Fingerprints Update, code: %i", httpCode);
				if (httpCode == HTTP_CODE_OK) {
					payload = http.getString();
					res = true;
				}
			} else {
				LOG_PRINTFLN(getContext(), "[manager] ERROR, Fingerprints Update, error: %s",
						http.errorToString(httpCode).c_str()
				);
			}
		}
		bool updated = false;
		if (payload.length()) {
			DynamicJsonBuffer jsonBuffer;
			JsonObject &root = jsonBuffer.parseObject(payload.begin());
			JsonArray &list = root[Strings::PAYLOAD_KEY_RESULTS];
			if (JsonArray::invalid() != list) {
				uint8_t idx = 0;
				// Set new values
				for(JsonArray::iterator it = list.begin();
						idx < getConfig().auth.getMaxFingerprintsQty() && it != list.end();
						++it)
				{
					JsonObject &item = *it;
					if (JsonObject::invalid() != item) {
						String value = item[Strings::PAYLOAD_KEY_VALUE];
						if (value.length()) {
							LOG_PRINTFLN(getContext(), "[manager] Fingerprint: %s", value.c_str());
							if (!getConfig().auth.fingerprints[idx].equals(value)) {
								getConfig().auth.fingerprints[idx] = value;
								updated = true;
							}
							++idx;
						}
					}
				}
				// Reset empty slots
				updated = getConfig().auth.resetFingerprints(idx) || updated;
			}
		}
		if (updated) {
			getConfig().store(getContext(), getConfigStorage());
		}
		return res;
	}

	/** Rotates fingerprints if current one is not valid anymore. */
	RotateFingerprintsStatus::Type rotateServerFingerprints() {
		LOG_PRINTFLN(getContext(), "[manager] Rotate fingerprints");
		bool updated = false;
		{
			WiFiClientSecure client;
			if (client.connect(getConfig().SERVER_ADDR, getConfig().SERVER_HTTPS_PORT)) {
				bool verified = false;
				for (uint8_t i = 0; i < getConfig().auth.getMaxFingerprintsQty(); ++i) {
					String &value = getConfig().auth.fingerprints[i];
					if (value.length() && client.verify(value.c_str(), getConfig().SERVER_ADDR)) {
						LOG_PRINTFLN(getContext(), "[manager] Switch to fingerprint: %s", value.c_str());
						verified = true;
						updated = getConfig().auth.resetFingerprints(0, i);
						break;
					}
				}
				if (!verified) {
					return RotateFingerprintsStatus::ERROR_VERIFY;
				}
			} else {
				LOG_PRINTFLN(getContext(), "[manager] ERROR, Rotate fingerprints: can't connect");
				return RotateFingerprintsStatus::ERROR;
			}
		}
		if (updated) {
			getConfig().store(getContext(), getConfigStorage());
		}
		return RotateFingerprintsStatus::OK;
	}

	/** Authenticate */
	AuthenticateStatus::Type authenticate() {
		AuthenticateStatus::Type res = AuthenticateStatus::ERROR;
		String payload;
		// Send authentication request
		{
			String url = Util::makeUrl(Strings::URL_MODEL_TOKEN,
					getConfig().SERVER_ADDR, getConfig().SERVER_HTTPS_PORT
			);
			CharArrayBuffer<> reqPayload;
			{
				DynamicJsonBuffer jsonBuffer;
				JsonObject &root = jsonBuffer.createObject();
				root[Strings::USERNAME] = getId();
				root[Strings::PASSWORD] = getId();
				root.printTo(reqPayload.get(), reqPayload.size());
			}
			HTTPClient http;
			http.begin(url, getConfig().auth.fingerprints[0]);
			http.addHeader(Strings::HEADER_CONTENT_TYPE, Strings::MIME_TYPE_APP_JSON);
			int httpCode = http.POST(reqPayload.get());
			if (httpCode > 0) {
				LOG_PRINTFLN(getContext(), "[manager] Authentication, code: %i", httpCode);
				switch(httpCode) {
					case HTTP_CODE_OK:
						payload = http.getString();
						break;
					case HTTP_CODE_FORBIDDEN:
						res = AuthenticateStatus::ERROR_FORBIDDEN;
						break;
					default:
						break;
				}
			} else {
				LOG_PRINTFLN(getContext(), "[manager] ERROR, Authentication, error: %s",
						http.errorToString(httpCode).c_str()
				);
			}
		}
		// Process authentication response
		bool updated = false;
		if (payload.length()) {
			{
				DynamicJsonBuffer jsonBuffer;
				JsonObject &root = jsonBuffer.parseObject(payload.begin());
				if (JsonObject::invalid() != root) {
					const char *v = root[Strings::TOKEN];
					if (v) {
						LOG_PRINTFLN(getContext(), "[manager] Token: %s", v);
						res = AuthenticateStatus::OK;
						if (!getConfig().auth.token.equals(v)) {
							getConfig().auth.token = v;
							updated = true;
						}
					}
				} else {
					LOG_PRINTFLN(getContext(), "[config] ERROR, Authentication failure: can't pars");
				}
			}
		}
		if (updated) {
			getConfig().store(getContext(), getConfigStorage());
		}
		return res;
	}

	/** Checks for the updates when it's time to check. */
	bool check() {
		if (!isUpdatesTime()) return true;
		bool res = false;
		getHttpUpdate().rebootOnUpdate(true);
		if (isServerFingerprint()) {
			if (checkServerFingerprintsUpdate()) {
				AuthenticateStatus::Type authStatus = AuthenticateStatus::OK;
				if (!isAuthenticated()) {
					authStatus = authenticate();
					if (AuthenticateStatus::ERROR_FORBIDDEN == authStatus) {
						sendSos("Forbidden authentication");
					}
				}
				if (AuthenticateStatus::OK == authStatus) {
					switch(checkFirmwareUpdate()) {
						case HTTP_UPDATE_NO_UPDATES:
							res = true;
							break;
						case HTTP_UPDATE_OK:
							// Wait the reboot
							break;
						default:
							sendSos("Failed FW update");
							break;
					}
				}
			} else {
				if (rotateServerFingerprints() == RotateFingerprintsStatus::ERROR_VERIFY) {
					sendSos("Bad fingerprints");
				}
			}
		} else {
			// Device is not paired with network
			switch(checkFirmwareUpdateNotSecure()) {
				case HTTP_UPDATE_NO_UPDATES:
					checkServerFingerprintsUpdate(false);
					break;
				case HTTP_UPDATE_OK:
					// Wait the reboot
					break;
				default:
					uint32_t retryTm = getConfig().NET_CONNECT_ERROR_RETRY_TM_MS;
					idle(retryTm);
			}
		}
		return res;
	}

	/** Send error message using HTTP */
	void sendSos(const char *msg) {
		// TODO: send error using HTTP
	}

	bool isAuthenticated() {
		return getConfig().auth.token.length();
	}

	bool isServerFingerprint() {
		return getConfig().auth.fingerprints[0].length();
	}

	//// GETTERS ////
	const String& getId() const {
		return mId;
	}

	const String& getName() const {
		return mName;
	}

	Context& getContext() {
		return mCtx;
	}

	CONFIG_T& getConfig() {
		return mConfig;
	}

	EspStorage& getConfigStorage() {
		return mConfigStorage;
	}

	EspLpm& getLpm() {
		return mLpm;
	}

	Time::EspClock& getClock() {
		return mClock;
	}

	HwUart& getHwUart() {
		return mHwUart;
	}

	EspHttpUpdate& getHttpUpdate() {
		return mHttpUpdate;
	}

private:
	String											mId;
	String											mName;
	Context											mCtx;
	CONFIG_T										mConfig;
	EspStorage										mConfigStorage;
	EspLpm											mLpm;
	uint32_t*										mLpmData;
	uint32_t										mLpmDataSize;
	Time::EspClock									mClock;
	HwUart											mHwUart;
	EspHttpUpdate									mHttpUpdate;

	void setupConfigMode() {
		//// SETUP ////
		LOG_PRINTFLN(getContext(), "[setup] CONFIG mode");
		Butler::Arduino::CaptivePortal::WiFiConfig configPortal;
		const char* ssid = getName().c_str();
		configPortal.start(
				ssid,
				[&](Config::WiFiConfig& wifiConfig)
		{
			LOG_PRINTFLN(getContext(), "[setup] WiFi configuration arrived via HTTP");
			bool changed = false;
			// Verify
			if (wifiConfig.isValid()) {
				changed = !getConfig().wifi.isEqual(wifiConfig);
				getConfig().wifi.set(wifiConfig);
			} else {
				LOG_PRINTFLN(getContext(), "[setup] ERROR, WiFi configuration isn't valid");
			}
			// Store
			if (changed) {
				LOG_PRINTFLN(getContext(), "[setup] Configuration changed");
				// Store
				getConfig().store(getContext(), getConfigStorage());
			}
			// Done
			restart();
		});
		//// SETUP END ////
		LOG_PRINTFLN(getContext(), "#################################");
		LOG_PRINTFLN(getContext(), "###  Butler device configurator");
		LOG_PRINTFLN(getContext(), "### AP SSID     : %s", ssid);
		LOG_PRINTFLN(getContext(), "### AP IP       : %s", WiFi.softAPIP().toString().c_str());
		LOG_PRINTFLN(getContext(), "#################################");

		//// EXECUTE ////
		while (true) {
			// Loop until configuration is set via WEB
			configPortal.process();
			yield();
		}
	}

	void setupNormalMode() {
		//// SETUP ////
		LOG_PRINTFLN(getContext(), "[setup] NORMAL mode");
		//// Initialize RESET pin ////
		pinMode(getConfig().CFG_RESET_PIN, INPUT_PULLUP);
		//// Check RESET request ////
		{
			int v = digitalRead(getConfig().CFG_RESET_PIN);
			Time::Timer timer(getClock(), getConfig().CFG_RESET_DELAY_MS);
			// Pin must be LOW for configured time
			while (v == LOW && !timer.expired()) {
				yield();
				v = digitalRead(getConfig().CFG_RESET_PIN);
			}
			// Pin is still LOW => reset
			if (v == LOW) {
				LOG_PRINTFLN(getContext(), "[setup] Reset configuration");
				getConfigStorage().reset();
				restart();
			}
		}
		//// LPM-CHECK ////
		getLpm().check(getConfig().CFG_RESET_PIN, mLpmData, mLpmDataSize);
		//// NETWORK ////
		WiFi.persistent(false);
		WiFi.mode(WIFI_STA);
		WiFi.hostname(getName().c_str());
		WiFi.begin(getConfig().wifi.ssid.c_str(), getConfig().wifi.passphrase.c_str());
		//// SETUP END ////
		LOG_PRINTFLN(getContext(), "#################################");
		LOG_PRINTFLN(getContext(), "###       Butler device");
		LOG_PRINTFLN(getContext(), "### ID          : %s", getId().c_str());
		LOG_PRINTFLN(getContext(), "### NAME        : %s", getName().c_str());
		LOG_PRINTFLN(getContext(), "#################################");
	}

	bool isUpdatesTime() {
		return true;
	}
};

}}

#endif // BUTLER_ARDUINO_ESP_MANAGER_H_
