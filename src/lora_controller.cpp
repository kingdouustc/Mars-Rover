#include "lora_controller.h"
#include "rover_config.h"
#include "FreeRTOS.h"
#include "esp_log.h"

#include <SPI.h>
#include <LoRa.h>

#define MAX_REGISTRATED_CALLBACKS 2

#define LORA_DATA_NOTIFICATION   1

#define LORA_PACKET_LENGTH        12
#define SCK                       5
#define MISO                      19
#define MOSI                      27
#define SS                        18
#define RST                       14
#define DIO0                      26

static const char* TAG = "LORA_CONTROLLER";

static void on_receive_isr(int packetSize);

static void lora_state_checker(void* args);
static void lora_receive_task(void* args);

static LoraControllerStatusCb* status_callbacks[MAX_REGISTRATED_CALLBACKS];
static uint8_t num_callbacks;
static bool is_initialized = false;
static uint16_t channel_values[RC_NUM_CHANNELS] = {0};
static LoraControllerStatus status = LORA_CONTROLLER_DISCONNECTED;
static uint8_t receive_buf[LORA_PACKET_LENGTH];

static uint32_t last_lora_data_ticks = 0;
static uint32_t last_lora_data_checked_ticks = 0;

static TaskHandle_t task_handle;

void lora_controller_init()
{
  assert(!is_initialized);
  BaseType_t task_create_status;
  
  memset(channel_values, 0, sizeof(channel_values));
  num_callbacks = 0;

  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);

  if (!LoRa.begin(868E6)) {
    ESP_LOGE(TAG, "Starting LoRa failed!");
    return;
  }
  LoRa.setFrequency(868E6);
  LoRa.setSpreadingFactor(6);
  LoRa.setSignalBandwidth(250E3);
  LoRa.setCodingRate4(5);

  LoRa.onReceive(on_receive_isr);

  LoRa.receive(LORA_PACKET_LENGTH);
  ESP_LOGI(TAG, "Starting LoRa OK!");

  task_create_status = xTaskCreate(lora_state_checker, "lora_state_checker", 2048, NULL, tskIDLE_PRIORITY, NULL);
  assert(task_create_status == pdPASS);

  task_create_status = xTaskCreate(lora_receive_task, "lora_receive", 2048, NULL, 2, &task_handle);
  assert(task_create_status == pdPASS);

  is_initialized = true;
}

void lora_controller_register_connection_callback(LoraControllerStatusCb* cb)
{
  assert(num_callbacks <= MAX_REGISTRATED_CALLBACKS);
  assert(is_initialized);
  status_callbacks[num_callbacks] = cb;
  num_callbacks++;
}

uint16_t lora_controller_get_val(uint8_t channel)
{
  uint16_t value;
  assert(status == LORA_CONTROLLER_CONNECTED);
  assert(channel < RC_NUM_CHANNELS);
  value = channel_values[channel];
  return value;
}

static void lora_receive_task(void* args)
{
  while (true) {
    uint32_t i = 0;
    uint32_t notification;
    
    assert(xTaskNotifyWait(LORA_DATA_NOTIFICATION, 0, &notification, portMAX_DELAY));
    assert(notification == LORA_DATA_NOTIFICATION);

    while (LoRa.available() && i < LORA_PACKET_LENGTH) {
      receive_buf[i] = (uint8_t)LoRa.read();
      i++;
    }

    if (i != LORA_PACKET_LENGTH) {
      printf("ERROR: Got more data than max packet length\n");
      return;
    }

    memcpy(channel_values, receive_buf, LORA_PACKET_LENGTH);
    //printf("Got: %d, %d \t %d, %d \t %d, %d\n", channel_values[0], channel_values[1], channel_values[2],  channel_values[3], channel_values[4], channel_values[5]);
  }
}

static void on_receive_isr(int packet_size)
{
  if (packet_size != LORA_PACKET_LENGTH) {
    ets_printf("ERROR: Unexpected packet size %d\n", packet_size);
    return;
  }

  last_lora_data_ticks = xTaskGetTickCountFromISR();

  BaseType_t shouldWakeUpTask;
  assert(xTaskNotifyFromISR(task_handle, LORA_DATA_NOTIFICATION, eSetValueWithOverwrite, &shouldWakeUpTask) == pdPASS);

  if (shouldWakeUpTask) {
    portYIELD_FROM_ISR();
  }
}

static void lora_state_checker(void* args)
{
  while (true) {
    if (last_lora_data_ticks > last_lora_data_checked_ticks) {
      if (status == LORA_CONTROLLER_DISCONNECTED) {
        status = LORA_CONTROLLER_CONNECTED;
        for (uint8_t i = 0; i < num_callbacks; i++) {
          status_callbacks[i](LORA_CONTROLLER_CONNECTED);
        }
      }
    } else {
      if (status == LORA_CONTROLLER_CONNECTED) {
        status = LORA_CONTROLLER_DISCONNECTED;
        for (uint8_t i = 0; i < num_callbacks; i++) {
          status_callbacks[i](LORA_CONTROLLER_DISCONNECTED);
        }
      }
    }
    last_lora_data_checked_ticks = xTaskGetTickCount();
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}