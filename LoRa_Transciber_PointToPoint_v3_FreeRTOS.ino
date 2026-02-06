/*
 * ---------------------------------------------------------------------------
 * ASIGNATURA: SISTEMAS DE SENSORES | MAESTRÍA IOT
 * PLATAFORMA: Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262)
 * IMPLEMENTACIÓN: FreeRTOS (Dual Core)
 * AUTOR: MGTI. Saul Isai Soto Ortiz
 * * OBJETIVO PEDAGÓGICO: 
 * Pasar de un código secuencial (loop) a un Sistema Operativo en Tiempo Real.
 * Usamos "Multitarea Cooperativa" dividiendo el trabajo en 3 obreros (Tareas).
 * ---------------------------------------------------------------------------
 */

#include <RadioLib.h>
#include <Wire.h>
#include "SSD1306Wire.h"

// ==========================================
// CONFIGURACIÓN DE HARDWARE
// ==========================================
// Pines específicos para Heltec V3
SX1262 radio = new Module(8, 14, 12, 13);
SSD1306Wire display(0x3c, 17, 18);
#define VEXT_PIN 21 
#define LED_PIN  35 

// Configuración de la "Estación de Radio"
#define FREQUENCY     915.0
#define BANDWIDTH     125.0
#define SPREAD_FACTOR 8
#define CODING_RATE   5

// ==========================================
// OBJETOS DEL SISTEMA OPERATIVO (Las Herramientas)
// ==========================================

// 1. MANEJADORES (Los identificadores de nuestros empleados)
TaskHandle_t TaskHandle_TX = NULL;
TaskHandle_t TaskHandle_RX = NULL;
TaskHandle_t TaskHandle_Display = NULL;

// 2. MUTEX (La "Llave del Baño" o "Token")
// La radio es un recurso único. Si dos tareas intentan usarla a la vez, 
// el sistema falla. El Mutex asegura que solo quien tenga la llave la use.
SemaphoreHandle_t mutexRadio;

// 3. SEMÁFORO BINARIO (El "Timbre de Puerta")
// Sirve para que la radio despierte a la CPU solo cuando llega un mensaje.
SemaphoreHandle_t semaforoISR_RX;

// 4. COLA (La "Cinta Transportadora")
// Permite enviar datos desde la Tarea de Radio (Core 0) a la Tarea de Pantalla (Core 1)
// sin que se peleen por la memoria.
QueueHandle_t colaDisplay;

// Estructura del paquete que viaja por la cinta transportadora
struct DatosDisplay {
  byte remitente;
  char mensaje[50]; 
  float rssi;
};

// ==========================================
// VARIABLES GLOBALES (Protocolo)
// ==========================================
byte dir_local   = 0xD3; // MI NOMBRE
byte dir_destino = 0xC1; // A QUIEN ESCRIBO
byte id_msjLoRa  = 0;
byte bufferPaquete[256]; // Caja de envío protegida por Mutex

// ==========================================
// INTERRUPCIÓN (ISR) - El "Timbre"
// ==========================================
// Esta función corre a velocidad luz cuando el chip LoRa recibe algo.
// NO debe tener lógica compleja, solo avisar.
#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
void setFlag(void) {
  // "Damos" el semáforo (Tocamos el timbre) para despertar al Obrero RX
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(semaforoISR_RX, &xHigherPriorityTaskWoken);
  
  // Si el Obrero RX es muy importante, dejamos lo que estamos haciendo para atenderlo
  if(xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

// ==========================================
// TAREA 1: EL OBRERO RECEPTOR (RX)
// Ubicación: Core 0 | Prioridad: ALTA
// Misión: Estar dormido hasta que suene el timbre, leer rápido y volver a dormir.
// ==========================================
void Tarea_RX_LoRa(void *parameter) {
  for(;;) {
    // 1. ESTADO DORMIDO: Espera infinita (portMAX_DELAY) hasta recibir el semáforo.
    // No consume CPU mientras espera.
    if (xSemaphoreTake(semaforoISR_RX, portMAX_DELAY) == pdTRUE) {
      
      // 2. Pedimos la "Llave de la Radio" (Mutex)
      // Esperamos máximo 100 ticks si está ocupada.
      if (xSemaphoreTake(mutexRadio, (TickType_t)100) == pdTRUE) {
        
        int packetLen = radio.getPacketLength();
        int state = radio.readData(bufferPaquete, packetLen);
        
        if (state == RADIOLIB_ERR_NONE) {
           // --- PROCESAMIENTO ---
           byte destino = bufferPaquete[0];
           byte remite  = bufferPaquete[1];
           
           // Filtro: ¿Es para mí?
           if (destino == dir_local || destino == 0xFF) {
              
              // Empaquetamos los datos para enviarlos a la otra oficina (Display)
              DatosDisplay datos;
              datos.remitente = remite;
              datos.rssi = radio.getRSSI();
              
              // Copiamos el mensaje texto
              String strMsg = "";
              for(int i=4; i < packetLen; i++) strMsg += (char)bufferPaquete[i];
              strMsg.toCharArray(datos.mensaje, 50);

              // Ponemos el paquete en la CINTA TRANSPORTADORA (Queue)
              xQueueSend(colaDisplay, &datos, 0);
              
              Serial.printf("[RX Core %d] Dato recibido y enviado a Pantalla\n", xPortGetCoreID());
              digitalWrite(LED_PIN, HIGH); vTaskDelay(50); digitalWrite(LED_PIN, LOW);
           }
        }
        
        // ¡IMPORTANTE! Volvemos a poner la oreja (Modo Escucha)
        radio.startReceive();
        
        // 3. Devolvemos la llave de la Radio para que otros la usen
        xSemaphoreGive(mutexRadio);
      }
    }
  }
}

// ==========================================
// TAREA 2: EL OBRERO TRANSMISOR (TX)
// Ubicación: Core 0 | Prioridad: MEDIA
// Misión: Despertar cada cierto tiempo, pedir la radio, enviar y dormir.
// ==========================================
void Tarea_TX_LoRa(void *parameter) {
  for(;;) {
    // Dormimos 6 segundos + un tiempo aleatorio (para evitar colisiones)
    vTaskDelay((6000 + random(0, 3000)) / portTICK_PERIOD_MS);

    // 1. Intentamos pedir la llave de la Radio
    // Si el Receptor la está usando, esperamos educadamente 500ms
    if (xSemaphoreTake(mutexRadio, (TickType_t)500) == pdTRUE) {
      
      String payload = "Temp: " + String(random(20, 30)) + "C";
      
      // --- ENCAPSULAMIENTO MANUAL ---
      int i = 0;
      bufferPaquete[i++] = dir_destino; // Byte 0
      bufferPaquete[i++] = dir_local;   // Byte 1
      bufferPaquete[i++] = id_msjLoRa++;// Byte 2
      bufferPaquete[i++] = (byte)payload.length(); // Byte 3
      for (int j=0; j<payload.length(); j++) bufferPaquete[i++] = payload.charAt(j);

      // ¡Fuego! (Esta operación bloquea unos milisegundos)
      radio.transmit(bufferPaquete, i);
      
      // Inmediatamente volvemos a escuchar para no perder mensajes
      radio.startReceive();
      
      // Devolvemos la llave
      xSemaphoreGive(mutexRadio);
      
      id_msjLoRa++;
      Serial.printf("[TX Core %d] Mensaje enviado.\n", xPortGetCoreID());
    } else {
      Serial.println("[TX] Radio ocupada (RX activo), intento luego.");
    }
  }
}

// ==========================================
// TAREA 3: EL ARTISTA DE PANTALLA (Display)
// Ubicación: CORE 1 | Prioridad: BAJA
// Misión: Recoger papeles de la cinta y dibujar. NO molesta a la radio.
// ==========================================
void Tarea_Pantalla(void *parameter) {
  DatosDisplay datosRecibidos;
  
  for(;;) {
    // Esperamos pacientemente a que caiga algo de la cinta transportadora.
    // Si no hay nada, esta tarea NO consume batería ni CPU.
    if (xQueueReceive(colaDisplay, &datosRecibidos, portMAX_DELAY) == pdTRUE) {
      
      // Dibujar en pantalla es lento, pero como estamos en Core 1,
      // la radio (en Core 0) sigue funcionando felizmente.
      display.clear();
      display.drawString(0, 0, "RX de: 0x" + String(datosRecibidos.remitente, HEX));
      display.drawString(0, 15, "Msg: " + String(datosRecibidos.mensaje));
      display.drawString(0, 30, "RSSI: " + String(datosRecibidos.rssi) + " dBm");
      display.drawString(0, 50, "Core Pantalla: " + String(xPortGetCoreID()));
      display.display();
    }
  }
}

// ==========================================
// CONFIGURACIÓN INICIAL (SETUP)
// ==========================================
void setup() {
  Serial.begin(115200);

  // 1. Despertar Heltec V3 (Vext ON)
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW); delay(100); digitalWrite(VEXT_PIN, HIGH);
  pinMode(LED_PIN, OUTPUT);

  // 2. Iniciar Periféricos
  display.init();
  display.flipScreenVertically();
  
  SPI.begin(9, 11, 10, 8);
  if (radio.begin(FREQUENCY, BANDWIDTH, SPREAD_FACTOR, CODING_RATE) != RADIOLIB_ERR_NONE) {
    Serial.println("Error Critico Radio"); while(1);
  }
  radio.setDio1Action(setFlag); // Conectar el timbre
  
  // 3. CREAR HERRAMIENTAS RTOS
  mutexRadio = xSemaphoreCreateMutex();        // Crear la llave única
  semaforoISR_RX = xSemaphoreCreateBinary();   // Crear el timbre
  colaDisplay = xQueueCreate(5, sizeof(DatosDisplay)); // Crear la cinta (max 5 items)

  // 4. CONTRATAR A LOS EMPLEADOS (Crear Tareas)
  
  // Obrero TX -> Core 0 (Mismo que la radio física)
  xTaskCreatePinnedToCore(Tarea_TX_LoRa, "TX_Task", 4096, NULL, 1, &TaskHandle_TX, 0);

  // Obrero RX -> Core 0 (Alta prioridad para no perder datos)
  xTaskCreatePinnedToCore(Tarea_RX_LoRa, "RX_Task", 4096, NULL, 2, &TaskHandle_RX, 0);

  // Artista Display -> Core 1 (Para no estorbar a la radio)
  xTaskCreatePinnedToCore(Tarea_Pantalla, "Display_Task", 4096, NULL, 1, &TaskHandle_Display, 1);

  // Arrancar en modo escucha
  radio.startReceive();
  Serial.println("Sistema FreeRTOS: Oficina Abierta.");
}

void loop() {
  // En FreeRTOS, el loop() es el "Jefe Ocioso".
  // Ya delegó todo el trabajo a las tareas, así que lo mandamos a casa.
  vTaskDelete(NULL); 
}