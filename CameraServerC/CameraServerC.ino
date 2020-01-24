#include <HardwareSerial.h>
#include <M5StickC.h>
#include <Arduino_CRC32.h>
#include "esp_http_server.h"
#include <WiFi.h>
#include <Preferences.h>

HardwareSerial serial_ext(2); // Serial from/to V via GROVE
Arduino_CRC32 crc32;
WiFiClient client;
unsigned long image_counter = 0;

void setup() {
  M5.begin();
  //Serial.begin(115200);
  //Serial.setTimeout(10);

  // M5StickV
  int baud = 1500000; // 115200 1500000 3000000 4500000
  serial_ext.begin(baud, SERIAL_8N1, 32, 33);
  serial_ext.setTimeout(10);
  serial_ext.setRxBufferSize(4096);

  while(1){
    Serial.printf("Connecting...\n");
    bool ok = wifi_connect(10*1000);
    if(ok){
      IPAddress ipAddress = WiFi.localIP();
      Serial.printf("%s\n", ipAddress.toString().c_str());
      break;
    }
  }
  startCameraServer();

  while(1){
    pingpong();

    sendStringToV("﻿cmd=RESET-REQ pixformat=RGB565 framesize=QQVGA\n");
    String line = readLineFromV_wait(10*1000);
    String resp = extract_string(line, "cmd");
    if(resp=="RESET-RESP"){
      String result = extract_string(line, "result");
      if(result == "OK"){
        break;
      }
      delay(1);
    }
  }
}

void loop() {
  delay(1);
//  uint8_t *buffer = NULL;
//  size_t size = 0;
//  bool ok = get_image_buffer(&buffer, &size);
//  if(ok){
//    Serial.printf("size=%d\n", size);
//    free_image_buffer(buffer);
//  }

  static unsigned long last_ms = 0;
  if(1*1000 <= millis()-last_ms || last_ms==0){
    update_display();
    last_ms = millis();
  }
}

bool get_image_buffer(uint8_t **p_buffer, size_t *p_size)
{
  String line;
  bool result = true;
  sendStringToV("cmd=SNAPSHOT-REQ format=JPEG quality=80\n");
  line = readLineFromV_wait(5*1000);
  if(extract_string(line, "cmd") != "SNAPSHOT-RESP"){
    pingpong();
    return false;
  }
  int seq  = extract_int(line, "seq");
  size_t size = extract_int(line, "size");
  //Serial.printf("  * seq=%d size=%d\n", seq, size);

  int retry_count = 0;
  int offset = 0;
  uint8_t *buffer = (uint8_t*)malloc(size);
  while(1){
    if(10<=retry_count){
      Serial.printf("  * FAILED\n");
      result = false;
      break;
    }
    int length = size-offset;
    int length_max = 2048;
    if(length_max<length){ length = length_max; }
    
    sendStringToV("cmd=DATA-REQ seq=%d offset=%d length=%d\n", seq, offset, length);
    line = readLineFromV_wait(100);
    if(extract_string(line, "cmd") != "DATA-RESP"){
      pingpong();
      retry_count += 1;
      continue;
    }
    int data_seq       = extract_int(line, "seq");
    int data_offset    = extract_int(line, "offset");
    int data_length    = extract_int(line, "length");
    String data_crc32  = extract_string(line, "crc32");
    //Serial.printf("  * data_seq=%d data_offset=%d data_length=%d data_crc32=%s\n", data_seq, data_offset, data_length, data_crc32.c_str());
    if(seq!=data_seq || offset!=data_offset || length!=data_length){
      pingpong();
      retry_count += 1;
      continue;
    }

    unsigned long start_ms = millis();
    int read_length = readFromV_wait(buffer+offset, data_length, 100);
    unsigned long elapsed_ms = millis()-start_ms;
    String read_crc32 = String(crc32.calc(buffer+offset, read_length));
    //Serial.printf("  * read_length=%d read_crc32=%s (elapsed_ms=%d)\n", read_length, read_crc32.c_str(), elapsed_ms);
    if(data_length!=read_length || data_crc32!=read_crc32){
      pingpong();
      retry_count += 1;
      continue;
    }

    offset += read_length;
    //Serial.printf("  * OK(%d/%d)\n", offset, size);
    if(offset==size){
      Serial.printf("  * ALL OK seq=%d size=%d\n", seq, size);
      image_counter++;
      result = true;
      break;
    }
    retry_count = 0;
  }

  if(result){
    *p_buffer = buffer;
    *p_size   = size;
    return true;
  }else{
    *p_buffer = NULL;
    *p_size   = 0;
    free(buffer);
    return false;
  }
}

void free_image_buffer(uint8_t *buffer){
  free(buffer);
}

void pingpong()
{
  while(1){
    while(serial_ext.available()){
      serial_ext.read();
    }

    String msg0 = String(micros());
    sendStringToV("cmd=PING msg=%s\n", msg0);

    String line = readLineFromV_wait(1000);
    if(extract_string(line, "cmd")=="PONG"){
      String msg1 = extract_string(line, "msg");
      if(msg0==msg1){
        Serial.printf("pingpong: OK\n");
        return;
      }else{
        Serial.printf("pingpong: NG\n");
      }
    }
    delay(1);
  }
}

size_t sendToV(uint8_t *buffer, size_t size)
{
  size_t wsize = serial_ext.write(buffer, size);
  //delay(1);
  return wsize;
}

template <typename ... Args>
void sendStringToV(const char *format, Args const & ... args) {
    //Serial.printf("C: ");
    //Serial.printf(format, args ...);
    serial_ext.printf(format, args ...);
    serial_ext.flush();
}

int readFromV(uint8_t *buffer, size_t size) {
  if (serial_ext.available()) {
    return serial_ext.readBytes(buffer, size);
  } else {
    return 0;
  }
}

int readFromV_wait(uint8_t *buffer, size_t size, unsigned long wait_ms) {
  unsigned long start_ms = millis();
  int read_length = 0;
  while(1){
    if (serial_ext.available()) {
      int l = serial_ext.readBytes(buffer+read_length, size-read_length);
      if(0<l){
        read_length += l;
      }
    }else{
      delay(1);
    }
    if(read_length==size || wait_ms<=millis()-start_ms){
      return read_length;
    }
  }
}

String readLineFromV(void) {
  if (serial_ext.available()) {
    return serial_ext.readStringUntil('\n');
  } else {
    return String("");
  }
}

String readLineFromV_wait(unsigned long wait_ms) {
  unsigned long start_ms = millis();
  while(1){
    if (serial_ext.available()) {
      String line = serial_ext.readStringUntil('\n');
      //Serial.printf("V: %s (elapsed_ms=%d)\n", line.c_str(), millis()-start_ms);
      return line;
    }else{
      delay(1);
    }
    if(wait_ms <= millis()-start_ms){
      Serial.printf("V: (timeout) (elapsed_ms=%d)\n", millis()-start_ms);
      return String("");
    }
  }
}

String extract_string(String s, char* key)
{
  char* c0 = strstr(s.c_str(), (String(key)+"=").c_str());
  if(c0 != NULL){
    c0 += String(key).length()+1;
    char* c1 = strstr(c0, " ");
    if(c1==NULL){ c1 = c0; while(*c1){ c1++; } }
    if(c1 != NULL){
      char buf[256];
      strncpy(buf, c0, c1-c0);
      buf[c1-c0] = 0;
      return String(buf);
    }
  }
  return String("");
}

int extract_int(String s, char* key)
{
  return atoi(extract_string(s, key).c_str());
}

float extract_float(String s, char* key)
{
  return atof(extract_string(s, key).c_str());
}


#define PART_BOUNDARY "aaaaaaaaaaaaaaaa"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
static const char* _INDEX_HTML = "<img src='/mjpg?framesize=TODO&quality=TODO' />";

httpd_handle_t httpd = NULL;

static esp_err_t index_handler(httpd_req_t *req){
  esp_err_t res = ESP_OK;
  res = httpd_resp_set_type(req, "text/html");
  if(res != ESP_OK){
    return res;
  }
  res = httpd_resp_send(req, _INDEX_HTML, strlen(_INDEX_HTML));
  return res;
}

static esp_err_t mjpg_handler(httpd_req_t *req){
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK){
    return res;
  }

  while(true){
    bool ok = get_image_buffer(&_jpg_buf, &_jpg_buf_len);
    if(!ok){ res = ESP_FAIL; }
    if(res == ESP_OK){
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    free_image_buffer(_jpg_buf);
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if(res != ESP_OK){
      break;
    }
  }
  return res;
}

void startCameraServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };
  httpd_uri_t mjpg_uri = {
    .uri       = "/mjpg",
    .method    = HTTP_GET,
    .handler   = mjpg_handler,
    .user_ctx  = NULL
  };
  
  if (httpd_start(&httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(httpd, &index_uri);
    httpd_register_uri_handler(httpd, &mjpg_uri);
  }
}

bool wifi_connect(int timeout_ms)
{
  if(WiFi.status() == WL_CONNECTED){
    return true;
  }
  uint8_t mac[6];
  WiFi.macAddress(mac);
  Serial.printf("mac: %s\n", mac_string(mac).c_str());

  Preferences preferences;
  char wifi_ssid[33];
  char wifi_key[65];

  preferences.begin("Wi-Fi", true);
  preferences.getString("ssid", wifi_ssid, sizeof(wifi_ssid));
  preferences.getString("key", wifi_key, sizeof(wifi_key));
  preferences.end();

  Serial.printf("wifi_ssid=%s\n", wifi_ssid);

  WiFi.begin(wifi_ssid, wifi_key);
  unsigned long start_ms = millis();
  bool connected = false;
  while(1){
    connected = WiFi.status() == WL_CONNECTED;
    if(connected || (start_ms+timeout_ms)<millis()){
      break;
    }
    delay(1);
  }
  return connected;
}

void wifi_disconnect(){
  if(WiFi.status() != WL_DISCONNECTED){
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
  }
}

String mac_string(const uint8_t *mac)
{
  char macStr[18] = { 0 };
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

void update_display()
{
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setRotation(3);
  M5.Lcd.setTextFont(1);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(WHITE, BLACK);

  M5.Lcd.setCursor(1, 1);

  M5.Lcd.printf("CameraServerC\n");

  RTC_TimeTypeDef time;
  RTC_DateTypeDef date;
  M5.Rtc.GetTime(&time);
  M5.Rtc.GetData(&date);
  M5.Lcd.printf("%04d-%02d-%02d %02d:%02d:%02d\n",
    date.Year, date.Month, date.Date,
    time.Hours, time.Minutes, time.Seconds
  );

  IPAddress ipAddress = WiFi.localIP();
  M5.Lcd.printf("IPAddr=%s\n", ipAddress.toString().c_str());

  static unsigned long prev_ms = 0;
  static int prev_counter = 0;
  if(0<prev_ms){
    float fps = 1000.0 * (image_counter-prev_counter) / (millis()-prev_ms);
    Serial.printf("fps=%5.2f\n", fps);
    M5.Lcd.printf("fps=%5.2f\n", fps);
  }
  prev_counter = image_counter;
  prev_ms = millis();
}
