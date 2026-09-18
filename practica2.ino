#include <BLEDevice.h>   // Librería principal de BLE, sirve para inicializar el ESP32 como dispositivo Bluetooth Low Energy
#include <BLEServer.h>   // Deja crear el "servidor" BLE al que se va a conectar el celular
#include <BLEUtils.h>    // Utilidades varias que usa BLE por dentro
#include <BLE2902.h>     // Descriptor que se necesita agregar para que las notificaciones (notify) funcionen

// UUIDs del servicio y la característica BLE, son como "direcciones" únicas para que el
// celular sepa a qué servicio/característica conectarse (se generan con cualquier generador de UUID)
#define SERVICE_UUID        "5a237299-a20c-4fb4-9feb-349936f07607"
#define CHARACTERISTIC_UUID "b5ce6bd8-71ad-4799-9385-a5b29d56d419"

// Puntero global a la característica BLE. Lo necesitamos accesible desde cualquier función
// para poder mandar notificaciones (notify) o cambiar su valor desde donde sea
BLECharacteristic *g_esp32BleCharacteristic;


// ============================================================
// PRACTICA 1 
// ============================================================

const int LED_PIN = 25;     // Pin donde está conectado el LED
const int BUTTON_PIN = 26;  // Pin donde está conectado el botón
const int POT_PIN = 34;     // Pin analógico donde está conectado el potenciómetro

// --- Estado del conmutador A/B ---
bool estadoA = true;                // true = Estado A (LED parpadeando), false = Estado B (LED apagado)
bool ledEncendido = false;          // Guarda si el LED está prendido o apagado en este momento
unsigned long ultimoCambioLED = 0;  // Marca de tiempo (millis) del último cambio de estado del LED

// --- Antirrebote (Debounce) ---
int ultimaLecturaCruda = LOW;         // Última lectura "sin filtrar" del botón
int estadoBotonEstable = LOW;         // Estado del botón ya confirmado/filtrado
unsigned long ultimoCambioBoton = 0;  // Cuándo cambió por última vez la lectura cruda
const unsigned long DEBOUNCE_MS = 50; // Tiempo que esperamos para confirmar que el click es real

// --- Control Remoto por Serial ---
bool sistemaHabilitado = true;     // Si está en false, todo el sistema queda apagado (comando OFF)
bool frecuenciaForzada = false;    // Si está en true, ya no se usa el pot, se usa el Hz fijo (comando BLINK:x)
float hzForzadoValor = 1.0;        // Frecuencia en Hz que se fuerza cuando frecuenciaForzada está activo

// --- Temporizador de Telemetría ---
unsigned long ultimaTelemetria = 0;                 // Última vez que se mandó telemetría por BLE
const unsigned long TELEMETRIA_INTERVALO_MS = 500;  // Cada cuánto se manda telemetría (500ms)

// --- Variables para READ_POT --- PRACTICA 2
bool leyendoPot = false;                            // true mientras estamos en modo "lectura de potenciómetro"
unsigned long inicioLecturaPot = 0;                 // Momento en que arrancó la lectura, para saber cuándo cortar
unsigned long ultimaLecturaPotEnviada = 0;          // Última vez que se mandó una lectura del pot
const unsigned long DURACION_READ_POT_MS = 5000;    // Cuánto dura el modo lectura (5 segundos)
const unsigned long INTERVALO_LECTURA_POT_MS = 300; // Cada cuánto se manda una lectura dentro de esos 5 segundos

// --- Variables para STATUS ---
String modoAnterior = "ON";            // Guarda el último modo reportado, para poder detectar cambios
unsigned long ultimoCambioModo = 0;    // Marca de tiempo de cuándo cambió el modo por última vez

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

// Manda el valor del potenciómetro por BLE cada cierto intervalo, durante los 5 segundos
// que dura el comando READ_POT. Pasado ese tiempo se apaga solo (no hace falta detenerlo a mano).
void manejarLecturaPot() {
  if (!leyendoPot) return; // si no nos pidieron leer el pot, no hacemos nada

  if (millis() - inicioLecturaPot >= DURACION_READ_POT_MS) {
    leyendoPot = false;   // ya pasaron los 5 segundos, cortamos el modo lectura
    Serial.println("READ_POT finalizado.");
    return;
  }

  if (millis() - ultimaLecturaPotEnviada >= INTERVALO_LECTURA_POT_MS) {
    ultimaLecturaPotEnviada = millis();

    int valorADC = analogRead(POT_PIN);   // leemos el valor crudo del pot (va de 0 a 4095)
    char buffer[30];
    snprintf(buffer, sizeof(buffer), "POT: %d", valorADC);  // armamos el texto a enviar

    g_esp32BleCharacteristic->setValue(buffer); // cargamos el valor en la característica BLE
    g_esp32BleCharacteristic->notify();         // avisamos al celular que hay un dato nuevo
    Serial.println(buffer);
  }
}

///----------

// Devuelve en texto en qué modo está el sistema ahora mismo, para usarlo en la telemetría y el status
String obtenerModoActual() {
  if (!sistemaHabilitado) return "OFF";   // el sistema está apagado (comando OFF)
  if (frecuenciaForzada) return "BLINK";  // parpadeando a una frecuencia fija (comando BLINK:x)
  return "ON";  // usando el potenciometro
}

/*
 * TELEMETRÍA VERSION BLE
 * Envía información de lo que está haciendo el ESP32 hacia la computadora, sin trabar el código.
 *
 * ¿Qué hace esta función?
 * Cada 500ms arma un mensaje con el estado actual (encendido/apagado, Estado A o B, valor del
 * potenciómetro y la frecuencia forzada si aplica) y lo manda por BLE con notify(), además de
 * imprimirlo por Serial para poder verlo mientras se debuguea desde la compu.
 * (Ahora mismo no se está llamando desde el loop(), quedó reemplazada por el sistema de STATUS)
 */
void enviarTelemetriaBLE() {
  if (millis() - ultimaTelemetria < TELEMETRIA_INTERVALO_MS) return; // todavía no toca mandar, salimos
  ultimaTelemetria = millis();

  int valorADC = analogRead(POT_PIN); // leemos el pot para reportar su valor actual

  char buffer[24];
  if (frecuenciaForzada) {
    // Formato: ON/OFF , A/B , valorADC , Hz forzado
    snprintf(buffer, sizeof(buffer), "%s,%s,%d,%.1f",
             sistemaHabilitado ? "ON" : "OFF",
             estadoA ? "A" : "B",
             valorADC,
             hzForzadoValor);
  } else {
    // Formato: ON/OFF , A/B , valorADC , "P" (de que se está usando el Potenciómetro)
    snprintf(buffer, sizeof(buffer), "%s,%s,%d,P",
             sistemaHabilitado ? "ON" : "OFF",
             estadoA ? "A" : "B",
             valorADC);
  }

  g_esp32BleCharacteristic->setValue(buffer); // cargamos el mensaje armado en la característica
  g_esp32BleCharacteristic->notify();         // lo mandamos al celular
  Serial.println(buffer);
}


// Arma y manda por BLE un resumen del estado actual (modo, segundos desde el último cambio
// de modo y valor del pot). Se usa cuando el celular pide "STATUS" y también cuando el modo
// cambia solo (ver revisarCambioDeModo).
void enviarStatus() {
  String modo = obtenerModoActual();
  int valorADC = analogRead(POT_PIN);
  unsigned long tiempoDesdeCambio = (millis() - ultimoCambioModo) / 1000; // segundos desde el último cambio de modo

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


// Se fija si el modo cambió desde la última vez que se revisó (por ejemplo de ON a BLINK) y,
// si cambió, manda un STATUS automáticamente para que el celular se entere sin tener que pedirlo
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


// Esta clase se dispara sola cada vez que el celular escribe algo en la característica BLE.
// Es básicamente el "recibidor" de comandos: agarra el texto que llega, lo limpia y decide
// qué hacer según cuál de los comandos conocidos sea.
class EventCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    String comando = String(characteristic->getValue().c_str()); // convertimos lo recibido a String
    comando.trim();          // sacamos espacios o saltos de línea de más
    comando.toUpperCase();   // pasamos todo a mayúsculas para no depender de cómo lo escriban

    if (comando.length() == 0) return; // si llegó vacío, no hay nada que hacer

    Serial.print("Comando BLE: ");
    Serial.println(comando);

    if (comando == "ON") {
      sistemaHabilitado = true;        // prende el sistema

    } else if (comando == "OFF") {
      sistemaHabilitado = false;       // apaga el sistema

    } else if (comando == "AUTO") {
      frecuenciaForzada = false;       // vuelve a usar el potenciómetro para la frecuencia

    } else if (comando.startsWith("BLINK:")) {
      float hz = comando.substring(6).toFloat(); // sacamos el número que viene después de "BLINK:"
      if (hz > 0) {
        frecuenciaForzada = true;
        hzForzadoValor = hz;           // forzamos esa frecuencia de parpadeo
      }
  //--------------------------------
    }else if (comando == "READ_POT") {
      leyendoPot = true;
      inicioLecturaPot = millis();
      ultimaLecturaPotEnviada = 0;
      Serial.println("Iniciando READ_POT (5 segundos)...");   // arranca el modo lectura de 5 segundos

    }else if (comando == "STATUS") {
      enviarStatus();   // manda el status apenas lo piden
    }
  }
};

//---------------------------

void setup() {
  Serial.begin(115200); // arrancamos el puerto serial para poder debuguear desde la compu

  pinMode(LED_PIN, OUTPUT);            // el LED es salida
  pinMode(BUTTON_PIN, INPUT_PULLDOWN); // el botón es entrada, con pulldown interno (reposa en LOW)

  BLEDevice::init("KETEIMPORTA-ESP32");                  // inicializa el BLE con el nombre que va a ver el celular
  BLEServer *esp32BleServer = BLEDevice::createServer();  // crea el "servidor" BLE
  BLEService *esp32BleService = esp32BleServer->createService(SERVICE_UUID); // crea el servicio con su UUID

  g_esp32BleCharacteristic = esp32BleService->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ  |   // el celular puede leer el valor actual
      BLECharacteristic::PROPERTY_WRITE |   // el celular puede escribir comandos
      BLECharacteristic::PROPERTY_NOTIFY    // el ESP32 puede avisar cambios con notify()
  );

  g_esp32BleCharacteristic->addDescriptor(new BLE2902()); // descriptor necesario para que notify() funcione
  g_esp32BleCharacteristic->setValue("Hola desde ESP32");  // valor inicial de la característica


  g_esp32BleCharacteristic->setCallbacks(new EventCallback()); // conectamos la clase que procesa los comandos

  esp32BleService->start(); // arrancamos el servicio BLE

  BLEAdvertising *esp32BleAdvertising = BLEDevice::getAdvertising();
  esp32BleAdvertising->addServiceUUID(SERVICE_UUID); // el ESP32 se anuncia con este UUID para que lo encuentren
  esp32BleAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising(); // empieza a anunciarse por BLE para que el celular lo pueda ver

  Serial.println("BLE listo y anunciando.");
}
//-------------------------------
void loop() {
  manejarBoton(); // siempre revisamos el botón, pase lo que pase

  if (!sistemaHabilitado) {
    // sistema apagado (comando OFF): LED apagado y listo
    digitalWrite(LED_PIN, LOW);
    ledEncendido = false;
  } else if (estadoA) {
    manejarParpadeoEstadoA(); // Estado A: LED parpadeando según el pot o la frecuencia forzada
  } else {
    // Estado B: LED apagado
    digitalWrite(LED_PIN, LOW);
    ledEncendido = false;
  }
//------------------------
  revisarCambioDeModo(); // avisa por BLE automáticamente si el modo cambió

  if (leyendoPot) {
    manejarLecturaPot();       // si estamos en modo READ_POT, vamos mandando lecturas del pot
  } else {
    //enviarTelemetriaBLE();
  }
}