//--------------------------------------------------------------------------------------
//  Faculdade:  UNISAL Sao Jose
//  Projeto:    TCC Pos Graduacao Eletronica Embarcada
//  Author:     Roberto Dalla Valle Filho
//  RA:         180008380
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
//Bibliotecas C

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

//--------------------------------------------------------------------------------------
//Bibliotecas do ESP32

#include <esp_wifi.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <esp_event_loop.h>
#include <esp_log.h>
#include <mqtt_client.h>
#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_adc_cal.h"

//--------------------------------------------------------------------------------------
//Bibliotecas do FreeRTOS

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

//--------------------------------------------------------------------------------------
//Bibliotecas de rede

#include <lwip\sockets.h>
#include <lwip\dns.h>
#include <lwip\netdb.h>

//--------------------------------------------------------------------------------------

#define ID1                   "DallaValle_ESP32_LEITURA_SENSOR"   //Identification
#define DEFAULT_VREF          1100                                //Vref Default
#define ADC_BITS              12                                  //Quantity of bit ESP32 ADC1
#define ADC_COUNTS            (1 << ADC_BITS)                     //Left Shift Raized 2 times .: ADC_COUNTS = 24

//--------------------------------------------------------------------------------------
//ESP32 ADC CONFIG

#if CONFIG_IDF_TARGET_ESP32
static esp_adc_cal_characteristics_t *adc_chars;
static const adc_channel_t channel = ADC_CHANNEL_6; //GPIO34 if ADC1, GPIO14 if ADC2
static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
#elif CONFIG_IDF_TARGET_ESP32S2
static const adc_channel_t channel = ADC_CHANNEL_6; // GPIO7 if ADC1, GPIO17 if ADC2
static const adc_bits_width_t width = ADC_WIDTH_BIT_13;
#endif
static const adc_atten_t atten = ADC_ATTEN_DB_11; //11 dB attenuation (ADC_ATTEN_DB_11) gives full-scale voltage 3.9 V (see note below)
static const adc_unit_t unit = ADC_UNIT_1;
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Declarações de Variaveis
//--------------------------------------------------------------------------------------
int sampleI;
unsigned int samples;

double Irms;
double filteredI;                //Filtered_ is the raw analog value minus the DC offset
double offsetI;                  //Low-pass filter output
double ICAL;                     //Calibration coefficient. Need to be set in order to obtain accurate results
double sqI, sumI;

float kwhValue = 0.0;
float sumKwh = 0.0;
float sumCost = 0.0;
float costKwh = 0.0;

//--------------------------------------------------------------------------------------
// Protótipos das funções de calculo de Irms
//--------------------------------------------------------------------------------------
void currentCalibration(double _ICAL);
double getIrms(int NUMBER_OF_SAMPLES);

//--------------------------------------------------------------------------------------
/*handle do Queue*/
//--------------------------------------------------------------------------------------

QueueHandle_t xSensor_Control = 0;

//--------------------------------------------------------------------------------------
/* Variáveis para Armazenar o handle da Task */
//--------------------------------------------------------------------------------------
TaskHandle_t xPublishTask;
TaskHandle_t xSensorTask;

//--------------------------------------------------------------------------------------
/*Prototipos das Tasks*/
//--------------------------------------------------------------------------------------
void vPublishTask(void *pvParameter);
void vSensorTask(void *pvParameter);

//--------------------------------------------------------------------------------------
//Inicializacao de TAGs
//--------------------------------------------------------------------------------------
static const char *TAG = "MQTT_IOT";
static const char *TAG1 = "TASK";
static const char *TAG2 = "sensor";
static const char *TAG3 = "ADC_ESP32";

//--------------------------------------------------------------------------------------
// MQTT
//--------------------------------------------------------------------------------------
const char *mqtt_server = "192.168.1.101"; //IP do BBB MQTT broker
const char* mqtt_username = "rdalla"; // MQTT username
const char* mqtt_password = "vao1ca"; // MQTT password
const char* clientID = "DallaValleESP32"; // MQTT client ID
const char *topic_mqtt_cmd = "/home/command";///currentmonitor";
const char* irms_topic = "home/sensor/irms";
const char* kwh_topic = "home/sensor/kwh";
const char* cost_topic = "home/sensor/cost";
char mqtt_buffer[128];

//--------------------------------------------------------------------------------------
/* estrutura de dados para o sensor */
//--------------------------------------------------------------------------------------
struct sensor
{
  double irms;
  float kwh;
  float cost;
} sensorCurrent;

//--------------------------------------------------------------------------------------

#if CONFIG_IDF_TARGET_ESP32
static void check_efuse(void)
{
  //Check TP is burned into eFuse
  if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK)
  {
    ESP_LOGI(TAG3, "eFuse Two Point: Supported\n");
  }
  else
  {
    ESP_LOGI(TAG3, "eFuse Two Point: NOT supported\n");
  }

  //Check Vref is burned into eFuse
  if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK)
  {
    ESP_LOGI(TAG3, "eFuse Vref: Supported\n");
  }
  else
  {
    ESP_LOGI(TAG3, "eFuse Vref: NOT supported\n");
  }
}

static void print_char_val_type(esp_adc_cal_value_t val_type)
{
  if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP)
  {
    ESP_LOGI(TAG3, "Characterized using Two Point Value\n");
  }
  else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF)
  {
    ESP_LOGI(TAG3, "Characterized using eFuse Vref\n");
  }
  else
  {
    ESP_LOGI(TAG3, "Characterized using Default Vref\n");
  }
}
#endif
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
//Referencia para saber status de conexao
//--------------------------------------------------------------------------------------

static EventGroupHandle_t wifi_event_group;
const static int CONNECTION_STATUS = BIT0;

//--------------------------------------------------------------------------------------
//Cliente MQTT
//--------------------------------------------------------------------------------------

esp_mqtt_client_handle_t mqtt_client;

//--------------------------------------------------------------------------------------
// Funcao Calibracao do Sensor de Corrente
//--------------------------------------------------------------------------------------

void currentCalibration(double _ICAL)
{
  ICAL = _ICAL;
  offsetI = ADC_COUNTS >> 1;
  int adjust = 0;
  
  while (adjust < 250)
  {
    getIrms(1480);
    adjust++;

  }

}

//--------------------------------------------------------------------------------------
// Funcao de acquisicao do valor Irms
//--------------------------------------------------------------------------------------

double getIrms(int NUMBER_OF_SAMPLES)
{
  samples = NUMBER_OF_SAMPLES;
  int SupplyVoltage = 3300; //3V3 power supply ESP32

  sampleI = 0;

  for (unsigned int n = 0; n < samples; n++)
  {
    sampleI = adc1_get_raw((adc1_channel_t)channel);

    // Digital low pass filter extracts the 2.5 V or 1.65 V dc offset,
    // then subtract this - signal is now centered on 0 counts.
    offsetI = (offsetI + (sampleI - offsetI) / ADC_COUNTS);
    filteredI = sampleI - offsetI;

    // Root-mean-square method current
    // 1) square current values
    sqI = filteredI * filteredI;
    // 2) sum
    sumI += sqI;
  }

  double I_RATIO = ICAL * ((SupplyVoltage / 1000.0) / (ADC_COUNTS));
  Irms = I_RATIO * sqrt(sumI / samples);

  //Reset accumulators
  sumI = 0;
  //--------------------------------------------------------------------------------------

  return Irms;
}

//--------------------------------------------------------------------------------------
//Tasks FreeRTOS
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
/* Task de Publish MQTT */
//--------------------------------------------------------------------------------------
void vPublishTask(void *pvParameter)
{

  struct sensor sensorReceived;

  while(1)
  {

    if (!xQueueReceive(xSensor_Control, (void *)&sensorReceived, 3000))
    {
      ESP_LOGI(TAG, "Falha ao receber o valor da fila xSensor_Control.\n");
    }
    
    //JSON FORMAT IF NECESSARY: "{\"current\":\"%lf\"}"

    ESP_LOGI(TAG, "Enviando dados para o topico %s...", irms_topic);
    //Sanity check do mqtt_client antes de publicar
    snprintf(mqtt_buffer, 128, "%lf", sensorReceived.irms);
    esp_mqtt_client_publish(mqtt_client, irms_topic, mqtt_buffer, 0, 0, 0);

    ESP_LOGI(TAG, "Enviando dados para o topico %s...", kwh_topic);
    //Sanity check do mqtt_client antes de publicar
    snprintf(mqtt_buffer, 128, "%f", sensorReceived.kwh);
    esp_mqtt_client_publish(mqtt_client, kwh_topic, mqtt_buffer, 0, 0, 0);

    ESP_LOGI(TAG, "Enviando dados para o topico %s...", cost_topic);
    //Sanity check do mqtt_client antes de publicar
    snprintf(mqtt_buffer, 128, "%f", sensorReceived.cost);
    esp_mqtt_client_publish(mqtt_client, cost_topic, mqtt_buffer, 0, 0, 0);

    vTaskDelay(20000 / portTICK_PERIOD_MS);
  }
}

//--------------------------------------------------------------------------------------
/* Task do sensor SCT013 */
//--------------------------------------------------------------------------------------
void vSensorTask(void *pvParameter)
{

  
  currentCalibration(23);

  ESP_LOGI(TAG, "Iniciando task leitura sensor SCT013 50A/1V...");
  

  while (1)
  {

    ESP_LOGI(TAG, "Lendo dados de Consumo de Energia...\n");

    ESP_LOGI(TAG, "Calculando a corrente Irms...\n");
    sensorCurrent.irms = getIrms(1676);
    /*
    Useful values to use as a parameter into calcIrms( ) are:

    1 cycle of mains:
    112 for a 50 Hz system, or 93 for a 60 Hz system.

    The smallest ‘universal’ number:
    559 (100 ms, or 5 cycles of 50 Hz or 6 cycles of 60 Hz).

    The recommended monitoring period:
    1676 (300 ms).
    */

    //Consumed = (Pot[W]/1000) x hours...Sent a packet in a interval of 20 seconds .: kwh = ((Irms * Volts * 20 seconds) / (1000 * 3600))
    ESP_LOGI(TAG, "Calculando KWh Consumido...\n");
    //sensorCurrent.kwh = (float)((sensorCurrent.irms * 127.0 * 20) / (1000.0 * 3600.0));
    kwhValue = (float)((sensorCurrent.irms * 127.0 * 20) / (1000.0 * 3600.0));
    sumKwh += kwhValue;
    sensorCurrent.kwh = sumKwh;

    //Com o reajuste de 2020, preço por kWh na CPFL Paulista é em torno de R$ 0,85 por kWh para a tarifa residencial
    ESP_LOGI(TAG, "Calculando Custo por KWh Consumido...\n");
    //sensorCurrent.cost = sensorCurrent.kwh * 0.85;
    costKwh = sensorCurrent.kwh * 0.85;
    sumCost += costKwh;
    sensorCurrent.cost = sumCost;

    ESP_LOGI(TAG2, "Read IRMS Value: %lf (A) | kWh Value: %f (kWh) | Cost per kWh : R$ %f", sensorCurrent.irms, sensorCurrent.kwh, sensorCurrent.cost);

    if (!xQueueSend(xSensor_Control, (void *)&sensorCurrent, 3000))
    {
      ESP_LOGI(TAG, "\nFalha ao enviar o valor para a fila xSensor_Control.\n");
    }

    vTaskDelay(20000 / portTICK_RATE_MS);
  }
}

//--------------------------------------------------------------------------------------
//Callback para tratar eventos MQTT
//--------------------------------------------------------------------------------------
static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
  // Connect to MQTT Broker
  switch (event->event_id)
  {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "Conexao Realizada com Broker MQTT");
    esp_mqtt_client_subscribe(mqtt_client, topic_mqtt_cmd, 0);

    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "Desconexao Realizada com Broker MQTT");
    break;

  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "Subscribe Realizado com Broker MQTT");
    break;

  case MQTT_EVENT_PUBLISHED:
    ESP_LOGI(TAG, "Publish Realizado com Broker MQTT");
    break;

  //Evento de chegada de mensagens
  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "Dados recebidos via MQTT");
    /* Verifica se o comando recebido é válido */
    if (strncmp(topic_mqtt_cmd, event->topic, event->topic_len) == 0)
    {

      //readMessage(event->data);
      ESP_LOGI(TAG, "OK Analisada...");
    }
    else
    {
      ESP_LOGI(TAG, "Topico invalido");
    }
    break;

  case MQTT_EVENT_ERROR:
    ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
    break;

  default:
    break;
  }
  return ESP_OK;
}

//--------------------------------------------------------------------------------------
//Callback para tratar eventos WiFi
//--------------------------------------------------------------------------------------
static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{

  switch (event->event_id)
  {
  case SYSTEM_EVENT_STA_START:
    esp_wifi_connect(); //inicia conexao Wi-Fi
    break;

  case SYSTEM_EVENT_STA_GOT_IP:
    xEventGroupSetBits(wifi_event_group, CONNECTION_STATUS); // "seta" status de conexao
    break;

  case SYSTEM_EVENT_STA_DISCONNECTED:
    esp_wifi_connect();                                        //tenta conectar de novo
    xEventGroupClearBits(wifi_event_group, CONNECTION_STATUS); //limpa status de conexao
    break;

  default:
    break;
  }
  return ESP_OK;
}

//--------------------------------------------------------------------------------------
//Inicializacao WiFi
//--------------------------------------------------------------------------------------
static void wifi_init(void)
{

  tcpip_adapter_init();
  wifi_event_group = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  wifi_config_t wifi_config = {
      .sta = {
          .ssid = "Moura Valle",   //a ssid da sua rede wifi
          .password = "00519335damore!", //o password da sua rede wifi
      }};

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); //ESP32 em modo station
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_LOGI(TAG, "Iniciando Conexao com Rede WiFi...");
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "Conectando...");
  xEventGroupWaitBits(wifi_event_group, CONNECTION_STATUS, false, true, portMAX_DELAY);
}

//--------------------------------------------------------------------------------------
//Inicializacao MQTT Service
//--------------------------------------------------------------------------------------
static void mqtt_init(void)
{
  const esp_mqtt_client_config_t mqtt_cfg = {
      .event_handle = mqtt_event_handler,
      .client_id = "DallaValleESP32",
      .username = mqtt_username,
      .password = mqtt_password,
      .host = mqtt_server,
      .port = 1883
  };

  //Inicializa cliente mqtt
  mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_start(mqtt_client);

}

//--------------------------------------------------------------------------------------
//APP_MAIN
//--------------------------------------------------------------------------------------
void app_main()
{

#if CONFIG_IDF_TARGET_ESP32
  //Check if Two Point or Vref are burned into eFuse
  check_efuse();
#endif

  //Configure ADC
  if (unit == ADC_UNIT_1)
  {
    adc1_config_width(width);
    adc1_config_channel_atten(channel, atten);
  }

#if CONFIG_IDF_TARGET_ESP32
  //Characterize ADC
  adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, adc_chars);
  print_char_val_type(val_type);
#endif

  ESP_LOGI(TAG, "Iniciando ESP32 IoT App...");
 
  // Setup de logs de outros elementos
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);

  // Inicializacao da NVS = Non-Volatile-Storage (NVS)
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  wifi_init();
  mqtt_init();

  //criação de fila do xSensor_Control (vSensorTask <--> vPublishTask)
  xSensor_Control = xQueueCreate(10, sizeof(struct sensor));
  if (xSensor_Control == NULL)
  {
    ESP_LOGI(TAG1, "Erro na criação da Queue.\n");
  }


  if (xTaskCreate(&vPublishTask, "vPublishTask", configMINIMAL_STACK_SIZE + 4096, NULL, 5, xPublishTask) != pdTRUE)
  {
    ESP_LOGE("Erro", "error - nao foi possivel alocar vPublishTask.\n");
    while (1);
  }

  if (xTaskCreate(&vSensorTask, "vSensorTask", configMINIMAL_STACK_SIZE + 4096, NULL, 5, xSensorTask) != pdTRUE)
  {
    ESP_LOGE("Erro", "error - nao foi possivel alocar vSensorTask.\n");
    while (1);
  }

  while (true)
  {
    vTaskDelay(pdMS_TO_TICKS(3000)); // Delay
  }

}
