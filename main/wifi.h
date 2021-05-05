#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_netif.h>
#include <esp_https_ota.h>

ESP_EVENT_DECLARE_BASE(WIFI_OTA_EVENTS);

enum
{
	WIFI_CONNECTING,
	WIFI_CONNECTED,
	WIFI_DISCONNECTED,
	WIFI_OTA_STARTED,
	WIFI_OTA_APP_DESC,
	WIFI_OTA_READ,
	WIFI_OTA_FAILED,
	WIFI_OTA_SUCCESS,
};

typedef struct
{
	const char* ssid;
	const char* password;
	int retries;
	esp_event_loop_handle_t eventloop;

	struct
	{
		const char* url;
		const char* pem;
	} ota;
} wifi_t;

int wifi_ota_init(wifi_t* wifi);
int wifi_ota_update(wifi_t* wifi);
int wifi_ota_deinit(wifi_t* wifi);

