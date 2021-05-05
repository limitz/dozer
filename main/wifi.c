#include "wifi.h"

static EventGroupHandle_t s_wifi_evt_group;

#define WIFI_CONN_BIT BIT0
#define WIFI_DONE_BIT BIT1
#define WIFI_FAIL_BIT BIT2

ESP_EVENT_DEFINE_BASE(WIFI_OTA_EVENTS);

static void wifi_post_event(wifi_t* wifi, int event, void* data, int len)
{
	if (wifi->eventloop)
	{
		esp_event_post_to(wifi->eventloop, WIFI_OTA_EVENTS, event, data, len, 10);
	}
	else 
	{
		esp_event_post(WIFI_OTA_EVENTS, event, data, len, 10);
	}
}

int wifi_ota_update(wifi_t* wifi)
{
	esp_err_t rc;
	esp_https_ota_handle_t handle;
	esp_app_desc_t* desc = NULL;

	do
	{
		esp_http_client_config_t http_config = {
			.url = wifi->ota.url,
			.cert_pem = (char*) wifi->ota.pem,
		};

		esp_https_ota_config_t ota_config = {
			.http_config = &http_config,
		};

		rc = esp_https_ota_begin(&ota_config, &handle);
		if (ESP_OK != rc) break;
		wifi_post_event(wifi, WIFI_OTA_STARTED, NULL, 0);

		desc = (esp_app_desc_t*) malloc(sizeof(esp_app_desc_t));
		if (!desc) { rc = -ENOMEM; break; }

		rc = esp_https_ota_get_img_desc(handle, desc);
		if (ESP_OK != rc) break;
		wifi_post_event(wifi, WIFI_OTA_APP_DESC, desc, sizeof(esp_app_desc_t));
		free(desc);

		for (;;)
		{
			rc = esp_https_ota_perform(handle);
			if (ESP_OK == rc) break;
			if (ESP_ERR_HTTPS_OTA_IN_PROGRESS == rc)
			{
				int read = esp_https_ota_get_image_len_read(handle);
				wifi_post_event(wifi, WIFI_OTA_READ,
						&read, sizeof(int));
				continue;
			}
			break;
		}
	} while (0);
		

	if (ESP_OK == rc)
	{
		if (esp_https_ota_is_complete_data_received(handle))
		{
			rc = esp_https_ota_finish(handle);
		}
		else rc = ESP_FAIL;
	}
	wifi_post_event(wifi, ESP_OK == rc ? WIFI_OTA_SUCCESS : WIFI_OTA_FAILED,
			&rc, sizeof(int));
	return rc;
}

static void wifi_task(void* arg)
{
	wifi_t* wifi = (wifi_t*) arg;

	EventBits_t bits = {0};
	wifi_config_t wifi_config = {0};
	memcpy(wifi_config.sta.ssid, wifi->ssid, sizeof(wifi_config.sta.ssid));
	memcpy(wifi_config.sta.password, wifi->password, sizeof(wifi_config.sta.password));

	ESP_ERROR_CHECK( esp_wifi_disconnect() );
	ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
	ESP_ERROR_CHECK( esp_wifi_connect() );

	for (;;)
	{
		bits = xEventGroupWaitBits(
			s_wifi_evt_group,
			WIFI_FAIL_BIT | WIFI_CONN_BIT | WIFI_DONE_BIT,
			true, false, portMAX_DELAY);
		if (bits & WIFI_CONN_BIT)
		{
			ESP_LOGI(__func__, "CONNECTED");
		}
		if (bits & WIFI_DONE_BIT)
		{
			ESP_LOGW(__func__, "DONE");
			vTaskDelete(NULL);
		}
		if (bits & WIFI_FAIL_BIT)
		{
			ESP_LOGE(__func__, "FAILED");
			vTaskDelete(NULL);
		}
	}
}

static void wifi_handler(void* arg, esp_event_base_t event, int32_t id, void* data)
{
	wifi_t* wifi = (wifi_t*) arg;
	if (WIFI_EVENT == event) switch (id)
	{
		case WIFI_EVENT_STA_START:
			xTaskCreate(wifi_task, "wifi task", 4096, wifi, 3, NULL);
			break;
		case WIFI_EVENT_STA_DISCONNECTED:
			xEventGroupClearBits(s_wifi_evt_group, WIFI_CONN_BIT);
			if (wifi->retries > 0)
			{
				wifi->retries--;
				esp_wifi_connect();
			}
			else 
			{
				xEventGroupSetBits(s_wifi_evt_group, WIFI_FAIL_BIT);
			}
			break;
	}

	if (IP_EVENT == event) switch (id)
	{
		case IP_EVENT_STA_GOT_IP:
			xEventGroupSetBits(s_wifi_evt_group, WIFI_CONN_BIT);
			break;
	}
}

int wifi_ota_init(wifi_t* wifi)
{
	tcpip_adapter_init();
	s_wifi_evt_group = xEventGroupCreate();
	esp_event_loop_create_default();

	wifi->retries = 5;
	wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&config) );
	ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_handler, wifi) );
	ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_handler, wifi) );
	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
	ESP_ERROR_CHECK( esp_wifi_start() );

	wifi_post_event(wifi, WIFI_CONNECTING, NULL, 0);

	EventBits_t bits = xEventGroupWaitBits(s_wifi_evt_group,
			WIFI_CONN_BIT | WIFI_FAIL_BIT,
			false, false, portMAX_DELAY);

	if (bits & WIFI_CONN_BIT)
	{
		wifi_post_event(wifi, WIFI_CONNECTED, NULL, 0);
		return ESP_OK;
	}
	if (bits & WIFI_FAIL_BIT)
	{
		wifi_post_event(wifi, WIFI_DISCONNECTED, NULL, 0);
		return ESP_FAIL;
	}
	return ESP_FAIL;
}

int wifi_ota_deinit(wifi_t* wifi)
{
	esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_handler);
	esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_handler);
	vEventGroupDelete(s_wifi_evt_group);
	esp_wifi_stop();
	return ESP_OK;
}
