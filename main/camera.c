#include "camera.h"

// variables
static const char* TAG = "CAMERA";
static volatile int flash_time = 1000;
static volatile int flash_mode = 0; // TODO: race condition entre execute (escritura) y capture (lectura)

tCameraStatus init_camera(void) {
    camera_config_t config = {
        .ledc_channel = LEDC_CHANNEL_0,
        .ledc_timer   = LEDC_TIMER_0,
        .pin_d0       = Y2_GPIO_NUM,
        .pin_d1       = Y3_GPIO_NUM,
        .pin_d2       = Y4_GPIO_NUM,
        .pin_d3       = Y5_GPIO_NUM,
        .pin_d4       = Y6_GPIO_NUM,
        .pin_d5       = Y7_GPIO_NUM,
        .pin_d6       = Y8_GPIO_NUM,
        .pin_d7       = Y9_GPIO_NUM,
        .pin_xclk     = XCLK_GPIO_NUM,
        .pin_pclk     = PCLK_GPIO_NUM,
        .pin_vsync    = VSYNC_GPIO_NUM,
        .pin_href     = HREF_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,
        .pin_pwdn     = PWDN_GPIO_NUM, // para apagar la camara??
        .pin_reset    = RESET_GPIO_NUM,
        .xclk_freq_hz = 20000000, // frecuencia del sensor
        .pixel_format = PIXFORMAT_JPEG, // tipo de foto
        .frame_size   = FRAMESIZE_UXGA, // resolucion (mas mejor)
        .jpeg_quality = 12, // grado de compresion (menos mejor)
        .fb_count     = 1, // cuantos buffers, osea imagenes podemos almacenar simultaneamente
        .fb_location  = CAMERA_FB_IN_PSRAM, // guardamos las imagenes en la psram (importante tenerlo activado desde menuconfig)
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY // guardar cuando esta vacio el buffer
    };

    ESP_LOGI(TAG, "Inicializando camara");
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error iniciando camara: 0x%x", err);
        return CAMERA_INIT_ERR;
    }

    ESP_LOGI(TAG, "Camara inicializada correctamente");
    return CAMERA_OK;
}



tCameraStatus capture_photo(camera_fb_t** fb) {
    if (flash_mode == 1) {
        flash(fb);
    }
    else {
        *fb = esp_camera_fb_get();
    }
    
    if (!(*fb)) {
        ESP_LOGE(TAG, "Error al capturar la imagen");
        return CAMERA_CAPTURE_ERR;
    }
    return CAMERA_OK;
}

void flash(camera_fb_t** fb) {
    gpio_set_level(FLASH_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(flash_time / 2));
    for (int i = 0; i < 4; i++) {
        *fb = esp_camera_fb_get();
        if (*fb == NULL) continue;
        esp_camera_fb_return(*fb);
        *fb = NULL;
    }
    *fb = esp_camera_fb_get();
    vTaskDelay(pdMS_TO_TICKS(flash_time / 2));
    gpio_set_level(FLASH_PIN, 0);
}

tCameraStatus change_flash_mode(int mode) {
    if (mode != 0 && mode != 1) return FLASH_PARAM_ERR;
    flash_mode = mode;
    return CAMERA_OK;
}

tCameraStatus change_flash_time(int time) {
    if (time <= 0) return FLASH_TIME_PARAM_ERR;
    flash_time = time;
    return CAMERA_OK;
}

int get_flash_mode() {
    return flash_mode;
}

int get_flash_time() {
    return flash_time;
}




