//Bibliotecas C

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

//Bibliotecas do ESP32

#include <esp_wifi.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <esp_event_loop.h>
#include <esp_log.h>
#include <mqtt_client.h>
#include <esp_adc_cal.h>
#include "driver/adc.h"
#include "driver/gpio.h"

//Bibliotecas do FreeRTOS

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

//Bibliotecas de rede

#include <lwip\sockets.h>
#include <lwip\dns.h>
#include <lwip\netdb.h>


// Defines de referencia para o codigo
//#define DHTPIN                4
//#define DHTTYPE               DHT22
//#define LED_CONNECTION_STATUS 2
//#define GPIO_COMMAND_CTRL     18

#define ID1                   "DallaValle_ESP32_LEITURA_SENSOR"

/* Estrutura que contem as informacoes para calibracao */
esp_adc_cal_characteristics_t adc_cal;

/*handle do Semaforo*/
//SemaphoreHandle_t xMutex = 0;

/*handle do Queue*/
QueueHandle_t xSensor_Control = 0;

/* Variáveis para Armazenar o handle da Task */
TaskHandle_t xPublishTask;
TaskHandle_t xSensorTask;

/*Prototipos das Tasks*/
void vPublishTask(void *pvParameter);
void vSensorTask(void *pvParameter);

//Inicializacao de TAGs
static const char *TAG = "MQTT_IOT";
static const char *TAG1 = "TASK";
static const char *TAG2 = "sensor";

// MQTT
const char* mqtt_server = "192.168.1.103";//IP do BBB MQTT broker
const char* mqtt_username = "rdalla"; // MQTT username
const char* mqtt_password = "vao1ca"; // MQTT password
const char* clientID = "dallavalle"; // MQTT client ID
const char *topic_mqtt_cmd = "/home/command";///currentmonitor";
const char *topic_mqtt_data = "/home";//airconditioning/currentmonitor";
//const char* temperature_topic = "home/livingroom/temperature";
char mqtt_buffer[128];

//Referencia para saber status de conexao
static EventGroupHandle_t wifi_event_group;
const static int CONNECTION_STATUS = BIT0;

//Cliente MQTT
esp_mqtt_client_handle_t mqtt_client;

//---------------------------------------------------------------------------
//Espaco para criacao de tasks para o FreeRTOS

/* Task de Publish MQTT */
void vPublishTask(void *pvParameter)
{


  while(1)
  {

    ESP_LOGI(TAG, "Enviando dados para o topico %s...", topic_mqtt_data);

    if (!xQueueReceive(xSensor_Control, &mqtt_buffer, 3000))
    {
      ESP_LOGI(TAG, "Falha ao receber o valor da fila xSensor_Control.\n");
    }


    //Sanity check do mqtt_client antes de publicar
    esp_mqtt_client_publish(mqtt_client, topic_mqtt_data, mqtt_buffer, 0, 0, 0);

    vTaskDelay(5000 / portTICK_PERIOD_MS);

  }
}



/* Task do sensor SCT013 */
void vSensorTask(void *pvParameter)
{
  
  int msg = 0; //numero de mensagens
  uint32_t adcValue = 0;
  double irms = 0.0;
  double current = 0.0;

  ESP_LOGI(TAG, "Iniciando task leitura SCT013...");

  while (1)
  {
    msg = msg + 1;
    ESP_LOGI(TAG, "Lendo dados de Consumo de Corrente...\n");
    
    /*
            Obtem a leitura RAW do ADC para depois ser utilizada pela API de calibracao
 
            Media simples de 100 leituras intervaladas com 30us
        */

    for (int i = 0; i < 200; i++)
    {
      adcValue += adc1_get_raw(ADC1_CHANNEL_6); //Obtem o valor RAW do ADC
      ets_delay_us(30);
    }
    adcValue /= 200;

    adcValue = esp_adc_cal_raw_to_voltage(adcValue, &adc_cal); //Converte e calibra o valor lido (RAW) para mV

    irms = (float)(adcValue / (float)(2047) * 30);

    current = irms/sqrt(2);

    ESP_LOGI(TAG2, "Read Current Value: %lf", current);

    snprintf(mqtt_buffer, 128, "{\"current\":\"%lf\"}", current);

    if (!xQueueSend(xSensor_Control, &mqtt_buffer, 3000))
    {
      ESP_LOGI(TAG, "\nFalha ao enviar o valor para a fila xSensor_Control.\n");
    }

    vTaskDelay(5000 / portTICK_RATE_MS);
  }
}

//---------------------------------------------------------------------------
//Callback para tratar eventos MQTT

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

//---------------------------------------------------------------------------
//Callback para tratar eventos WiFi

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


//---------------------------------------------------------------------------
//Inicializacao WiFi

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

//---------------------------------------------------------------------------
//Inicializacao MQTT Service

static void mqtt_init(void)
{
  const esp_mqtt_client_config_t mqtt_cfg = {
      .event_handle = mqtt_event_handler,
      .client_id = "dallavalle", //cada um use o seu!
      .username = mqtt_username,
      .password = mqtt_password,
      .host = mqtt_server,
      .port = 1883
  };

  //Inicializa cliente mqtt
  mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_start(mqtt_client);

}

//---------------------------------------------------------------------------
//APP_MAIN

void app_main()
{
  /*Configurando LED como saida*/
  //gpio_pad_select_gpio(LED_CONNECTION_STATUS);
  //gpio_set_direction(LED_CONNECTION_STATUS, GPIO_MODE_INPUT_OUTPUT); //GPIO_MODE_INPUT_OUTPUT , para possibilitar leitura do estado do LED

  /*Configurando botao como entrada*/
  //gpio_pad_select_gpio(GPIO_COMMAND_CTRL);
  //gpio_set_direction(GPIO_COMMAND_CTRL, GPIO_MODE_INPUT);

  //---------------------------------------------------------------------------
  /*Configurando ADC1 CHANNEL 0 e CHANNEL 3*/
  adc1_config_width(ADC_WIDTH_BIT_12);                        //Configura a resolucao
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11); //Configura a atenuacao (pino 34)
  adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_11); //Configura a atenuacao (pino 35)

  esp_adc_cal_value_t adc_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_cal); //Inicializa a estrutura de calibracao

  if (adc_type == ESP_ADC_CAL_VAL_EFUSE_VREF)
  {
    ESP_LOGI("ADC CAL", "Vref eFuse encontrado: %umV", adc_cal.vref);
  }
  else if (adc_type == ESP_ADC_CAL_VAL_EFUSE_TP)
  {
    ESP_LOGI("ADC CAL", "Two Point eFuse encontrado");
  }
  else
  {
    ESP_LOGW("ADC CAL", "Nada encontrado, utilizando Vref padrao: %umV", adc_cal.vref);
  }
  
  //---------------------------------------------------------------------------

  ESP_LOGI(TAG, "Iniciando ESP32 IoT App...");
  // Setup de logs de outros elementos
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);

  /* Inicializacao de ponteiros de dados JSON */
  //printed_sensor = malloc(100 * sizeof(int32_t));

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
  xSensor_Control = xQueueCreate(10, sizeof(int));
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
