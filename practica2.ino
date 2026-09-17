#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID        "5a237299-a20c-4fb4-9feb-349936f07607"
#define CHARACTERISTIC_UUID "b5ce6bd8-71ad-4799-9385-a5b29d56d419"

BLECharacteristic *g_esp32BleCharacteristic;


// ============================================================
// PRACTICA 1 
// ============================================================

const int LED_PIN = 25;
const int BUTTON_PIN = 26;
const int POT_PIN = 34;

// --- Estado del conmutador A/B ---
bool estadoA = true;
bool ledEncendido = false;
unsigned long ultimoCambioLED = 0;

// --- Antirrebote (Debounce) ---
int ultimaLecturaCruda = LOW;
int estadoBotonEstable = LOW;
unsigned long ultimoCambioBoton = 0;
const unsigned long DEBOUNCE_MS = 50;

// --- Control Remoto por Serial ---
bool sistemaHabilitado = true;     
bool frecuenciaForzada = false;    
float hzForzadoValor = 1.0;        

// --- Temporizador de Telemetría ---
unsigned long ultimaTelemetria = 0;
const unsigned long TELEMETRIA_INTERVALO_MS = 500; 

// --- Variables para READ_POT --- PRACTICA 2
bool leyendoPot = false;
unsigned long inicioLecturaPot = 0;
unsigned long ultimaLecturaPotEnviada = 0;
const unsigned long DURACION_READ_POT_MS = 5000;
const unsigned long INTERVALO_LECTURA_POT_MS = 300;

// --- Variables para STATUS ---
String modoAnterior = "ON";            
unsigned long ultimoCambioModo = 0;      

/*
 * LECTURA DEL BOTÓN (Filtro Antirrebote / Debounce)
 * Los botones físicos son de metal y hacen "falso contacto" cuando los presionas.
 * El ESP32 es tan rápido que leería esos falsos contactos como múltiples clics seguidos.
 * 
 * ¿Qué hace esta función?
 * 1. Espera 50ms para darle tiempo al metal de que deje de vibrar (Debounce por software).
 * 2. Si después de esos 50ms el botón sigue presionado, lo toma como un clic real y estable.
 * 3. Al confirmar el clic justo al presionar (Flanco de subida), alterna entre el Estado A 
 *    y el Estado B. A esta acción de cambiar de un estado a otro se le llama Conmutación o Toggle.
 */
void manejarBoton() {
  int lecturaCruda = digitalRead(BUTTON_PIN);

  if (lecturaCruda != ultimaLecturaCruda) {
    ultimoCambioBoton = millis();
  }

  if ((millis() - ultimoCambioBoton) > DEBOUNCE_MS) {
    if (lecturaCruda != estadoBotonEstable) {
      estadoBotonEstable = lecturaCruda;

      if (estadoBotonEstable == HIGH) {
        estadoA = !estadoA;
        Serial.printf("Boton: cambio a Estado %s\n", estadoA ? "A" : "B");
      }
    }
  }
  ultimaLecturaCruda = lecturaCruda;
}

/*
 * PARPADEO DEL LED SIN PAUSAR EL ESP32 (Temporización No Bloqueante)
 * Controla el LED sin usar delay(), lo que permite al ESP32 seguir escuchando al botón 
 * y al puerto serial al mismo tiempo.
 * 
 * ¿Qué hace esta función?
 * 1. Revisa si hay una frecuencia forzada por el comando de la computadora. Si no, lee el potenciómetro físico.
 * 2. Calcula el tiempo de espera necesario (Mapeo a milisegundos).
 * 3. Constantemente se pregunta: "¿Ya pasó el tiempo necesario?".
 *    - Si NO ha pasado, se sale rápido y deja que el programa siga su curso (Libera el procesador).
 *    - Si SÍ ya pasó el tiempo, prende o apaga el LED y anota la hora exacta del cambio (Timer Asíncrono).
 */
void manejarParpadeoEstadoA() {
  int retardoMs;

  if (frecuenciaForzada) {
    retardoMs = (int)(1000.0 / hzForzadoValor / 2.0);
  } else {
    int valorADC = analogRead(POT_PIN);
    retardoMs = map(valorADC, 0, 4095, 1000, 50);
  }

  if (millis() - ultimoCambioLED >= retardoMs) {
    ultimoCambioLED = millis();
    ledEncendido = !ledEncendido;
    digitalWrite(LED_PIN, ledEncendido);
  }
}

//---------------

void manejarLecturaPot() {
  if (!leyendoPot) return;

  if (millis() - inicioLecturaPot >= DURACION_READ_POT_MS) {
    leyendoPot = false;
    Serial.println("READ_POT finalizado.");
    return;
  }

  if (millis() - ultimaLecturaPotEnviada >= INTERVALO_LECTURA_POT_MS) {
    ultimaLecturaPotEnviada = millis();

    int valorADC = analogRead(POT_PIN);
    char buffer[30];
    snprintf(buffer, sizeof(buffer), "POT: %d", valorADC);

    g_esp32BleCharacteristic->setValue(buffer);
    g_esp32BleCharacteristic->notify();
    Serial.println(buffer);
  }
}

///----------

String obtenerModoActual() {
  if (!sistemaHabilitado) return "OFF";
  if (frecuenciaForzada) return "BLINK";
  return "ON";  // usando el potenciometro
}

/*
 * TELEMETRÍA VERSION BLE
 * Envía información de lo que está haciendo el ESP32 hacia la computadora, sin trabar el código.
 * 
 * ¿Qué hace esta función?
 * 
 */
void enviarTelemetriaBLE() {
  if (millis() - ultimaTelemetria < TELEMETRIA_INTERVALO_MS) return;
  ultimaTelemetria = millis();

  int valorADC = analogRead(POT_PIN);

  char buffer[24]; 
  if (frecuenciaForzada) {
    snprintf(buffer, sizeof(buffer), "%s,%s,%d,%.1f",
             sistemaHabilitado ? "ON" : "OFF",
             estadoA ? "A" : "B",
             valorADC,
             hzForzadoValor);
  } else {
    snprintf(buffer, sizeof(buffer), "%s,%s,%d,P",
             sistemaHabilitado ? "ON" : "OFF",
             estadoA ? "A" : "B",
             valorADC);
  }

  g_esp32BleCharacteristic->setValue(buffer);
  g_esp32BleCharacteristic->notify();
  Serial.println(buffer);
}


void enviarStatus() {
  String modo = obtenerModoActual();
  int valorADC = analogRead(POT_PIN);
  unsigned long tiempoDesdeCambio = (millis() - ultimoCambioModo) / 1000;

  char buffer[20];  
  if (modo == "BLINK") {
   void enviarStatus() {
  String modo = obtenerModoActual();
  int valorADC = analogRead(POT_PIN);
  unsigned long tiempoDesdeCambio = (millis() - ultimoCambioModo) / 1000;

  char buffer[20];  
  if (modo == "BLINK") {
    // Status,MODO,FRECUENCIA,TIEMPO,ADC
    snprintf(buffer, sizeof(buffer), "S,%s,%.1f,%lu,%d",
             modo.c_str(), hzForzadoValor, tiempoDesdeCambio, valorADC);
  } else {
    // Formato: S,MODO,TIEMPO,ADC
    snprintf(buffer, sizeof(buffer), "S,%s,%lu,%d",
             modo.c_str(), tiempoDesdeCambio, valorADC);
  }

  g_esp32BleCharacteristic->setValue(buffer);
  g_esp32BleCharacteristic->notify();
  Serial.println(buffer);
}
    snprintf(buffer, sizeof(buffer), "S,%s,%.1f,%lu,%d",
             modo.c_str(), hzForzadoValor, tiempoDesdeCambio, valorADC);
  } else {
    // Formato: S,MODO,TIEMPO,ADC
    snprintf(buffer, sizeof(buffer), "S,%s,%lu,%d",
             modo.c_str(), tiempoDesdeCambio, valorADC);
  }

  g_esp32BleCharacteristic->setValue(buffer);
  g_esp32BleCharacteristic->notify();
  Serial.println(buffer);
}


void revisarCambioDeModo() {
  String modoActual = obtenerModoActual();

  if (modoActual != modoAnterior) {
    modoAnterior = modoActual;
    ultimoCambioModo = millis();

    Serial.print("Cambio de modo detectado: ");
    Serial.println(modoActual);

    enviarStatus();   // notifica automaticamente el cambio
  }
}

///-------


class EventCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    String comando = String(characteristic->getValue().c_str());
    comando.trim();
    comando.toUpperCase();

    if (comando.length() == 0) return;

    Serial.print("Comando BLE: ");
    Serial.println(comando);

    if (comando == "ON") {
      sistemaHabilitado = true;

    } else if (comando == "OFF") {
      sistemaHabilitado = false;

    } else if (comando == "AUTO") {
      frecuenciaForzada = false;

    } else if (comando.startsWith("BLINK:")) {
      float hz = comando.substring(6).toFloat();
      if (hz > 0) {
        frecuenciaForzada = true;
        hzForzadoValor = hz;
      }
  //--------------------------------
    }else if (comando == "READ_POT") {         
      leyendoPot = true;
      inicioLecturaPot = millis();
      ultimaLecturaPotEnviada = 0;   
      Serial.println("Iniciando READ_POT (5 segundos)...");

    }else if (comando == "STATUS") {         
      enviarStatus();
    }
  }
};

//---------------------------

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);           
  pinMode(BUTTON_PIN, INPUT_PULLDOWN); 

  BLEDevice::init("KETEIMPORTA-ESP32");
  BLEServer *esp32BleServer = BLEDevice::createServer();
  BLEService *esp32BleService = esp32BleServer->createService(SERVICE_UUID);

  g_esp32BleCharacteristic = esp32BleService->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ  |
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_NOTIFY
  );

  g_esp32BleCharacteristic->addDescriptor(new BLE2902());
  g_esp32BleCharacteristic->setValue("Hola desde ESP32");


  g_esp32BleCharacteristic->setCallbacks(new EventCallback());

  esp32BleService->start();

  BLEAdvertising *esp32BleAdvertising = BLEDevice::getAdvertising();
  esp32BleAdvertising->addServiceUUID(SERVICE_UUID);
  esp32BleAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("BLE listo y anunciando.");
}
//-------------------------------
void loop() {
  manejarBoton();

  if (!sistemaHabilitado) {
    digitalWrite(LED_PIN, LOW);
    ledEncendido = false;
  } else if (estadoA) {
    manejarParpadeoEstadoA();
  }
//------------------------
  revisarCambioDeModo(); 

  if (leyendoPot) {
    manejarLecturaPot();
  } else {
    //enviarTelemetriaBLE();
  }
}