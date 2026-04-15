#ifndef CAMERA_H
#define CAMERA_H

#include "esp_camera.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define FLASH_PIN 4
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

typedef enum {
    CAMERA_OK = 0,
    CAMERA_CAPTURE_ERR = 1,
    FLASH_PARAM_ERR = 2,
    FLASH_TIME_PARAM_ERR = 3,
    CAMERA_INIT_ERR = 4,
} tCameraStatus;

tCameraStatus init_camera(void);
tCameraStatus capture_photo(camera_fb_t** fb);
void flash(camera_fb_t** fb);
tCameraStatus change_flash_mode(int mode);
tCameraStatus change_flash_time(int time);
int get_flash_mode();
int get_flash_time();


#endif /* CAMERA_H */