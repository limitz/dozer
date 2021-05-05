#include "wifi.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include <esp_http_server.h>
#include <esp_camera.h>

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static const char *TAG = "ws_echo_server";

static const camera_config_t cam_config = {
		.pin_pwdn = -1, 
                .pin_reset = -1, 
                .pin_xclk = 4,
                .pin_sscb_sda = 18, 
                .pin_sscb_scl = 23, 

                .pin_d7 = 36, 
                .pin_d6 = 37, 
                .pin_d5 = 38, 
                .pin_d4 = 39, 
                .pin_d3 = 35, 
                .pin_d2 = 26, 
                .pin_d1 = 13, 
                .pin_d0 = 34, 

                .pin_vsync = 5,
                .pin_href = 27, 
                .pin_pclk = 25, 

                .xclk_freq_hz = 20000000,
                .ledc_timer = LEDC_TIMER_0,
                .ledc_channel = LEDC_CHANNEL_0,
                .pixel_format = PIXFORMAT_JPEG,
                .frame_size = FRAMESIZE_QVGA,
                .jpeg_quality = 12, 
                .fb_count = 4,
};
/*
 * Structure holding server handle
 * and internal socket fd in order
 * to use out of request send
 */
struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

/*
 * async send function, which we put into the httpd work queue
 */
static void ws_async_send(void *arg)
{
	struct async_resp_arg *resp_arg = arg;
	int fd = resp_arg->fd;
	httpd_handle_t hd = resp_arg->hd;
    	
	camera_fb_t* fb = esp_camera_fb_get();
	if (fb)
	{
		ESP_LOGI(__func__, "W:%d H:%d LEN:%d FMT:%d", fb->width, fb->height, fb->len, fb->format);
	
		httpd_ws_frame_t ws_pkt = {
			.payload = fb->buf,
			.len = fb->len,
			.type = HTTPD_WS_TYPE_BINARY,
		};

		httpd_ws_send_frame_async(hd, fd, &ws_pkt);
		esp_camera_fb_return(fb);
	}
	free(resp_arg);
}

inline esp_err_t trigger_async_send(httpd_handle_t handle, httpd_req_t *req)
{
    struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
    resp_arg->hd = req->handle;
    resp_arg->fd = httpd_req_to_sockfd(req);
    return httpd_queue_work(handle, ws_async_send, resp_arg);
}

static esp_err_t index_handler(httpd_req_t *req)
{
	ESP_LOGI(__func__, "Requesting index");
	httpd_resp_send(req, (char*) index_html_start, index_html_end - index_html_start);
	return ESP_OK;
}

static esp_err_t cam_handler(httpd_req_t *req)
{
	camera_fb_t* fb = esp_camera_fb_get();
	if (fb)
	{
		ESP_LOGI(__func__, "W:%d H:%d LEN:%d FMT:%d", fb->width, fb->height, fb->len, fb->format);
		httpd_resp_set_type(req, "image/jpg");
		httpd_resp_send(req, (char*)fb->buf, fb->len);
		
		/*
		int chunksize = 4096;
		for (int i=0; i<fb->len; i+=chunksize)
		{
			int len = chunksize;
			if (i + chunksize > fb->len) len = fb->len - i;
			httpd_resp_send_chunk(req, ((char*)fb->buf)+i, len);
			ESP_LOGI(__func__, "Sent chunk of size %d", len);
		}
		httpd_resp_send_chunk(req, NULL, 0);
		*/
	}
	return ESP_OK;
}

/*
 * This handler echos back the received ws data
 * and triggers an async send if certain message received
 */
static esp_err_t echo_handler(httpd_req_t *req)
{
	uint8_t buf[128] = { 0 };
	httpd_ws_frame_t ws_pkt = {
		.payload = buf,
		.type = HTTPD_WS_TYPE_BINARY,
	};
	
	esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 128);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
		return ret;
	}

	if (ws_pkt.type == HTTPD_WS_TYPE_BINARY)
	{
		float* f = (float*) ws_pkt.payload;
		ESP_LOGW(__func__, "%0.02f %0.02f %0.02f %0.02f %0.02f %0.02f", f[0], f[1], f[2], f[3], f[4], f[5]);
		camera_fb_t* fb = esp_camera_fb_get();
		if (fb)
		{
			ESP_LOGI(__func__, "W:%d H:%d LEN:%d FMT:%d", fb->width, fb->height, fb->len, fb->format);
			ws_pkt.payload = fb->buf;
			ws_pkt.len = fb->len;
			ws_pkt.type = HTTPD_WS_TYPE_BINARY;

			httpd_ws_send_frame(req, &ws_pkt);
			esp_camera_fb_return(fb);
		}
		return ESP_OK;
	}
	    // return trigger_async_send(req->handle, req);
	    //return ret;
    
    ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    ESP_LOGI(TAG, "Packet type: %d", ws_pkt.type);
    
    ret = httpd_ws_send_frame(req, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
    }
    return ret;
}

static const httpd_uri_t ws = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = echo_handler,
        .user_ctx   = NULL,
        .is_websocket = true
};

static const httpd_uri_t root = {
	.uri        = "/",
	.method     = HTTP_GET,
	.handler    = index_handler,
	.user_ctx   = NULL,
};

static const httpd_uri_t cam = {
	.uri        = "/cam",
	.method     = HTTP_GET,
	.handler    = cam_handler,
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Registering the ws handler
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &ws);
        httpd_register_uri_handler(server, &root);
  	httpd_register_uri_handler(server, &cam);
	return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base, 
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}


void app_main(void)
{
    static httpd_handle_t server = NULL;

    wifi_t wifi = {
	    .ssid = "LimitzNet",
	    .password = "yqfpkmse",
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(wifi_ota_init(&wifi));

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection.
     */
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

	ESP_ERROR_CHECK( esp_camera_init(&cam_config) );
	


    /* Start the server for the first time */
    server = start_webserver();
}
