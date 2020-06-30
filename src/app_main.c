//Bibliotecas C

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

//Bibliotecas do ESP32

#include <esp_wifi.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <esp_event_loop.h>
#include <esp_log.h>
#include <mqtt_client.h>
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

/* Funções auxiliares */
void readMessage(char *data);

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
const char* mqtt_server = "192.168.1.101";//IP do BBB MQTT broker
const char* mqtt_username = "rdalla"; // MQTT username
const char* mqtt_password = "vao1ca"; // MQTT password
const char* clientID = "dallavalle"; // MQTT client ID
//const char *topic_mqtt_cmd = "/home/onoff";///currentmonitor";
const char *topic_mqtt_data = "/home";//airconditioning/currentmonitor";
//const char* temperature_topic = "home/livingroom/temperature";

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

    if (!xQueueReceive(xSensor_Control, &"COLOCAR_VARIAVEL_ADC_SENSOR", 3000))
    {
      ESP_LOGI(TAG, "Falha ao receber o valor da fila xSensor_Control.\n");
    }


    //Sanity check do mqtt_client antes de publicar
    esp_mqtt_client_publish(mqtt_client, topic_mqtt_data, "COLOCAR_VARIAVEL_ADC_SENSOR", 0, 0, 0);

    vTaskDelay(5000 / portTICK_PERIOD_MS); //PRECISA?????

  }
}



/* Task do sensor SCT013 */
void vSensorTask(void *pvParameter)
{
  setDHTgpio(DHTPIN);
  int msg = 0; //numero de mensagens
  int temperature = 0;
  int humidity = 0;
  cJSON *monitor;

  ESP_LOGI(TAG, "Iniciando task leitura SCT013...");

  while (1)
  {
    msg = msg + 1;

    ESP_LOGI(TAG, "Lendo dados de Consumo de Corrente...\n");
    //int ret = readDHT();
    //errorHandler(ret);
    //temperature = getTemperature();
    //humidity = getHumidity();

    /* Cria a estrutura de dados MONITOR a ser enviado por JSON */

    ESP_LOGI(TAG2, "Info data:%s\n", "COLOCAR_VARIAVEL_ADC_SENSOR");

    if (!xQueueSend(xSensor_Control, &printed_sensor, 3000))
    {
      ESP_LOGI(TAG, "\nFalha ao enviar o valor para a fila xSensor_Control.\n");
    }

    vTaskDelay(5000 / portTICK_RATE_MS); //???
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
    //esp_mqtt_client_subscribe(mqtt_client, topic_mqtt_cmd, 0);

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
