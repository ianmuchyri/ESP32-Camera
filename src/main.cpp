#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include "FS.h"     // SD Card ESP32
#include "SD_MMC.h" // SD Card ESP32

#define CAMERA_MODEL_AI_THINKER

#include "camera_pins.h"

static uint16_t stream_image_queue_length = 100;
static uint16_t store_image_queue_length = 100;

int pictureNumber = 0;
String path = "/" + String(pictureNumber) + ".jpg";

QueueHandle_t streamImageQueue;
QueueHandle_t storeImageQueue;

TaskHandle_t saveImageToSDCardTaskHandle = NULL;
TaskHandle_t streamImagesTaskHandle = NULL;
TaskHandle_t captureImagesTaskHandle = NULL;

const char *ssid = "Galaxy";
const char *password = "4321,dcba";

const char *websockets_server_host = "192.168.93.161";
const uint16_t websockets_server_port = 8080;

using namespace websockets;
WebsocketsClient client;

typedef struct
{
  size_t size;  // number of values used for filtering
  size_t index; // current value index
  size_t count; // value count
  int sum;
  int *values; // array to be filled with values
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size)
{
  memset(filter, 0, sizeof(ra_filter_t));

  filter->values = (int *)malloc(sample_size * sizeof(int));
  if (!filter->values)
  {
    return NULL;
  }
  memset(filter->values, 0, sample_size * sizeof(int));

  filter->size = sample_size;
  return filter;
}

static int ra_filter_run(ra_filter_t *filter, int value)
{
  if (!filter->values)
  {
    return value;
  }
  filter->sum -= filter->values[filter->index];
  filter->values[filter->index] = value;
  filter->sum += filter->values[filter->index];
  filter->index++;
  filter->index = filter->index % filter->size;
  if (filter->count < filter->size)
  {
    filter->count++;
  }
  return filter->sum / filter->count;
}

camera_fb_t *captureImage()
{
  camera_fb_t *fb = NULL;
  fb = esp_camera_fb_get();
  if (!fb)
  {
    Serial.println("Camera capture failed");
  }
  return fb;
}

void captureImages(void *parameter)
{
  static int droppedImagePackets = 0;
  camera_fb_t *fb = NULL;
  while (true)
  {

    fb = captureImage();

    if (xQueueSend(streamImageQueue, &fb, portMAX_DELAY) != pdTRUE || xQueueSend(storeImageQueue, &fb, portMAX_DELAY) != pdTRUE)
    {
      droppedImagePackets++;
    }

    Serial.println("Dropped Images" + droppedImagePackets);

    // yield to other task
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

void saveImageToSDCard(void *parameter)
{
  camera_fb_t *fb = NULL;

  while (true)
  {

    if (xQueueReceive(storeImageQueue, &fb, portMAX_DELAY) == pdTRUE)
    {
      Serial.println("Image received for SD card storage");
    }
    else
    {
      Serial.println("Failed to receive image");
    }

    // save image to SD Card
    File file = SD_MMC.open("/test.jpg", FILE_WRITE);
    if (!file)
      Serial.println("[-] Failed to open file for writing");
    else
    {
      file.write(fb->buf, fb->len);
      file.close();
      pictureNumber++;
      path = "/" + String(pictureNumber) + ".jpg";
    }
    // end save image to SD Card

    // yield to other task
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void streamImages(void *parameter)
{
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  camera_fb_t *fb = NULL;
  while (true)
  {
    if (xQueueReceive(streamImageQueue, &fb, portMAX_DELAY) == pdTRUE)
    {
      Serial.println("Image received for streaming ");
    }
    else
    {
      Serial.println("Failed to receive image");
    }

    _jpg_buf_len = fb->len;
    _jpg_buf = fb->buf;
    client.sendBinary((const char *)_jpg_buf, _jpg_buf_len);

    if (fb)
    {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    }
    else if (_jpg_buf)
    {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }

    // yield to other task
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void init_camera()
{
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  // init with high specs to pre-allocate larger buffers
  if (psramFound())
  {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  }
  else
  {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID)
  {
    s->set_vflip(s, 1);       // flip it back
    s->set_brightness(s, 1);  // up the brightness just a bit
    s->set_saturation(s, -2); // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  s->set_framesize(s, FRAMESIZE_QVGA);
}

void init_wifi()
{
  WiFi.begin(ssid, password);
  Serial.println("ssid: ");
  Serial.println(ssid);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  while (!client.connect(websockets_server_host, websockets_server_port, "/"))
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("[+] Websocket Connected!");
}

void init_sd_card()
{
  if (!SD_MMC.begin("/sdcard", true))
    Serial.println("[-] SD Card Mount Failed");
  else
    Serial.println("[+] SD Card Mount Success");

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE)
    Serial.println("[-] No SD Card attached");
  else
    Serial.println("[+] Found SD Card attached");
}

void setup()
{
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  init_camera();

  init_wifi();

  init_sd_card();

  // Create the queue
  streamImageQueue = xQueueCreate(10, sizeof(camera_fb_t));
  storeImageQueue = xQueueCreate(10, sizeof(camera_fb_t));

  ra_filter_init(&ra_filter, 20);

  // Create the tasks
  xTaskCreatePinnedToCore(saveImageToSDCard, "SaveImage", 10000, NULL, 1, &saveImageToSDCardTaskHandle, 0);
  xTaskCreatePinnedToCore(streamImages, "StreamImage", 10000, NULL, 1, &streamImagesTaskHandle, 1);
  xTaskCreatePinnedToCore(captureImages, "CaptureImage", 10000, NULL, 2, &captureImagesTaskHandle, 1);

  vTaskDelete(NULL);
}

void loop()
{
}
