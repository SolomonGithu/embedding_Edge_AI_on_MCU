/* Credits:
// This code is partialy based on the AsyncWebCamera.cpp file developed by GitHub user "me-no-dev"
// Repository to the original code:
// https://gist.github.com/me-no-dev/d34fba51a8f059ac559bf62002e61aa3
*/

typedef struct {
  camera_fb_t * fb;
  size_t index;
} camera_frame_t;

// stream content configuration
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
static const char * JPG_CONTENT_TYPE = "image/jpeg";

// init camera
bool initCamera() {
  bool camera_is_initialized = false;

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

  /* Frame sizes:
    FRAMESIZE_UXGA (1600 x 1200)
    FRAMESIZE_QVGA (320 x 240)
    FRAMESIZE_CIF (352 x 288)
    FRAMESIZE_VGA (640 x 480)
    FRAMESIZE_SVGA (800 x 600)
    FRAMESIZE_XGA (1024 x 768)
    FRAMESIZE_SXGA (1280 x 1024)
    FRAMESIZE_QCIF (176 x 144)
    FRAMESIZE_40x30,    // 40x30
    FRAMESIZE_64x32,    // 64x32
    FRAMESIZE_64x64,    // 64x64
    FRAMESIZE_QQCIF,    // 88x72
    FRAMESIZE_QQVGA,    // 160x120
    FRAMESIZE_QQVGA2,   // 128x160
    FRAMESIZE_QCIF,     // 176x144
  */
  config.frame_size = FRAMESIZE_QVGA,    //QQVGA-UXGA Do not use sizes above QVGA when not JPEG
  
  // PIXFORMAT_JPEG -> the camera module itself compresses the image before sending it to the ESP32-S3, which reduces RAM usage.
  // PIXFORMAT_GRAYSCALE -> Grayscale Images Are Uncompressed

  config.pixel_format = PIXFORMAT_JPEG; // for streaming
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 22; // Default = 12. Lower values = better quality, but larger file size
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    //return;
    camera_is_initialized = false;
  } else {
    camera_is_initialized = true;
  }

  sensor_t * s = esp_camera_sensor_get();
  
    // initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 0); // flip it back
    //s->set_brightness(s, 1); // up the brightness just a bit
    s->set_brightness(s, -2); // lower the brightness. ADDED
    s->set_contrast(s, 0);       // -2 to 2. ADDED
    //s->set_saturation(s, -2); // lower the saturation
    s->set_saturation(s, -2); // increase the saturation. ADDED
    }
    // drop down frame size for higher initial frame rate
    if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
    }
  

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

  // Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash(LED_GPIO_NUM);
#endif

  return camera_is_initialized;
}

// stream jpg
class AsyncJpegStreamResponse: public AsyncAbstractResponse {
  private:
    camera_frame_t _frame;
    size_t _index;
    size_t _jpg_buf_len;
    uint8_t * _jpg_buf;
    uint64_t lastAsyncRequest;
  public:
    AsyncJpegStreamResponse() {
      _callback = nullptr;
      _code = 200;
      _contentLength = 0;
      _contentType = STREAM_CONTENT_TYPE;
      _sendContentLength = false;
      _chunked = true;
      _index = 0;
      _jpg_buf_len = 0;
      _jpg_buf = NULL;
      lastAsyncRequest = 0;
      memset(&_frame, 0, sizeof(camera_frame_t));
    }
    ~AsyncJpegStreamResponse() {
      if (_frame.fb) {
        if (_frame.fb->format != PIXFORMAT_JPEG) {
          free(_jpg_buf);
        }
        esp_camera_fb_return(_frame.fb);
      }
    }
    bool _sourceValid() const {
      return true;
    }
    virtual size_t _fillBuffer(uint8_t *buf, size_t maxLen) override {
      size_t ret = _content(buf, maxLen, _index);
      if (ret != RESPONSE_TRY_AGAIN) {
        _index += ret;
      }
      return ret;
    }
    size_t _content(uint8_t *buffer, size_t maxLen, size_t index) {
      // available PSRAM and DRAM memory
      //size_t freeDRAM = heap_caps_get_free_size(MALLOC_CAP_8BIT);
      //size_t freePSRAM = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

      if (!_frame.fb || _frame.index == _jpg_buf_len) {
        if (index && _frame.fb) {
          uint64_t end = (uint64_t)micros();
          int fp = (end - lastAsyncRequest) / 1000;
          //log_printf("Size: %uKB, Time: %ums (%.1ffps)\n", _jpg_buf_len/1024, fp);
          //log_printf("PSRAM:%u, DRAM:%u\n",freePSRAM/1024, freeDRAM/1024);
          lastAsyncRequest = end;
          if (_frame.fb->format != PIXFORMAT_JPEG) {
            free(_jpg_buf);
          }
          esp_camera_fb_return(_frame.fb);
          _frame.fb = NULL;
          _jpg_buf_len = 0;
          _jpg_buf = NULL;
        }
        if (maxLen < (strlen(STREAM_BOUNDARY) + strlen(STREAM_PART) + strlen(JPG_CONTENT_TYPE) + 8)) {
          //log_w("Not enough space for headers");
          return RESPONSE_TRY_AGAIN;
        }
        // get frame
        _frame.index = 0;

        _frame.fb = esp_camera_fb_get();
        if (_frame.fb == NULL) {
          //log_e("Camera frame failed");
          return 0;
        }

        if (_frame.fb->format != PIXFORMAT_JPEG) {
          unsigned long st = millis();
          bool jpeg_converted = frame2jpg(_frame.fb, 80, &_jpg_buf, &_jpg_buf_len);
          if (!jpeg_converted) {
            //log_e("JPEG compression failed");
            esp_camera_fb_return(_frame.fb);
            _frame.fb = NULL;
            _jpg_buf_len = 0;
            _jpg_buf = NULL;
            return 0;
          }
          //log_i("JPEG: %lums, %uB", millis() - st, _jpg_buf_len);
        } else {
          _jpg_buf_len = _frame.fb->len;
          _jpg_buf = _frame.fb->buf;
        }

        // send boundary
        size_t blen = 0;
        if (index) {
          blen = strlen(STREAM_BOUNDARY);
          memcpy(buffer, STREAM_BOUNDARY, blen);
          buffer += blen;
        }
        // send header
        size_t hlen = sprintf((char *)buffer, STREAM_PART, JPG_CONTENT_TYPE, _jpg_buf_len);
        buffer += hlen;
        // send frame
        hlen = maxLen - hlen - blen;
        if (hlen > _jpg_buf_len) {
          maxLen -= hlen - _jpg_buf_len;
          hlen = _jpg_buf_len;
        }
        memcpy(buffer, _jpg_buf, hlen);
        _frame.index += hlen;
        return maxLen;
      }

      size_t available = _jpg_buf_len - _frame.index;
      if (maxLen > available) {
        maxLen = available;
      }
      memcpy(buffer, _jpg_buf + _frame.index, maxLen);
      _frame.index += maxLen;

      return maxLen;
    }
};
