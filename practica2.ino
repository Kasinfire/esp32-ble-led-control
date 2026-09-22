#include <BLEDevice.h>  //inicializar el ESP32 como dispositivo Ble
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h> //Descriptor que se necesita agregar para notify

#define SERVICE_UUID        "5a237299-a20c-4fb4-9feb-349936f07607"
#define CHARACTERISTIC_UUID "b5ce6bd8-71ad-4799-9385-a5b29d56d419"

BLECharacteristic *g_esp32BleCharacteristic;


// ============================================================
// PRACTICA 1 (base)
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

// --- Control Remoto por BLE ---
bool sistemaHabilitado = true;
bool frecuenciaForzada = false;
float hzForzadoValor = 1.0;

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

        char buffer[20];
        snprintf(buffer, sizeof(buffer), "S,%s", estadoA ? "A" : "B");
        g_esp32BleCharacteristic->setValue(buffer);
        g_esp32BleCharacteristic->notify();
      }
    }
  }                                   
  ultimaLecturaCruda = lecturaCruda;
}
/*
 * PARPADEO DEL LED SIN PAUSAR EL ESP32 (Temporización No Bloqueante)
 * Controla el LED sin usar delay(), lo que permite al ESP32 seguir escuchando al botón
 * y las conexiones BLE al mismo tiempo.
 *
 * ¿Qué hace esta función?
 * 1. Revisa si hay una frecuencia forzada por comando BLE. Si no, lee el potenciómetro físico.
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

/*
 * LECTURA DEL POTENCIÓMETRO BAJO DEMANDA (READ_POT)
 * Cuando llega el comando READ_POT por BLE, no queremos quedarnos "atorados"
 * leyendo el potenciómetro por 5 segundos completos, porque eso bloquearía
 * el botón, el parpadeo del LED y hasta la propia conexión BLE.
 *
 * ¿Qué hace esta función?
 * 1. Si no se activó READ_POT (leyendoPot == false), no hace nada y se sale de inmediato.
 * 2. Revisa si ya se cumplieron los 5 segundos de la ventana de lectura (DURACION_READ_POT_MS).
 *    Si ya pasaron, apaga la bandera leyendoPot y las notificaciones se detienen solas.
 * 3. Si todavía estamos dentro de la ventana, verifica si ya pasó el intervalo entre
 *    lecturas (INTERVALO_LECTURA_POT_MS). Si sí, lee el potenciómetro, arma el mensaje
 *    y lo envía por notificación BLE (Timer Asíncrono, igual que el parpadeo del LED).
 */

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

/*
 * DETERMINAR EL MODO ACTUAL DEL SISTEMA
 * En vez de repetir la misma lógica de if/else en varias partes del código
 * para saber "en qué está" el sistema, se centralizó todo aquí en una sola función.
 *
 * ¿Qué hace esta función?
 * 1. Si el sistema está deshabilitado (comando OFF), el modo es "OFF" sin importar nada más.
 * 2. Si no está deshabilitado pero hay una frecuencia forzada por BLE (comando BLINK:x),
 *    el modo es "BLINK".
 * 3. Si ninguna de las anteriores aplica, el LED está parpadeando según el potenciómetro,
 *    así que el modo es "ON".
 */
String obtenerModoActual() {
  if (!sistemaHabilitado) return "OFF";
  if (frecuenciaForzada) return "BLINK";
  return "ON";  // usando el potenciometro
}

/*
 * ARMAR Y ENVIAR EL MENSAJE DE ESTADO POR BLE
 * Como los paquetes BLE tienen tamaño limitado, en vez de mandar una frase larga
 * se usa un formato corto y compacto que el cliente (celular) puede interpretar fácil.
 *
 * ¿Qué hace esta función?
 * 1. Obtiene el modo actual con obtenerModoActual() y lee el potenciómetro.
 * 2. Calcula cuánto tiempo ha pasado desde el último cambio de modo (en segundos).
 * 3. Si el modo es BLINK, arma el mensaje incluyendo también la frecuencia forzada,
 *    quedando algo como "S,BLINK,2.0,15,1840". En cualquier otro modo, se omite la
 *    frecuencia y queda como "S,ON,42,2103".
 * 4. Envía el mensaje armado por notificación BLE y lo imprime también por Serial
 *    para poder revisarlo durante las pruebas.
 */

void enviarStatus() {
  String modo = obtenerModoActual();
  int valorADC = analogRead(POT_PIN);
  unsigned long tiempoDesdeCambio = (millis() - ultimoCambioModo) / 1000;

  char buffer[40]; 
  if (modo == "BLINK") {
    snprintf(buffer, sizeof(buffer), "S,%s,%.1f,%lu,%d",
             modo.c_str(), hzForzadoValor, tiempoDesdeCambio, valorADC);
  } else {
    snprintf(buffer, sizeof(buffer), "S,%s,%lu,%d",
             modo.c_str(), tiempoDesdeCambio, valorADC);
  }

  g_esp32BleCharacteristic->setValue(buffer);
  g_esp32BleCharacteristic->notify();
  Serial.println(buffer);
}


/*
 * DETECCIÓN AUTOMÁTICA DE CAMBIO DE MODO
 * Esta función corre en cada vuelta del loop(), comparando constantemente el modo
 * actual contra el que se tenía guardado la vuelta anterior (Polling de estado).
 *
 * ¿Qué hace esta función?
 * 1. Obtiene el modo actual con obtenerModoActual().
 * 2. Mientras el modo no cambie respecto a modoAnterior, no hace absolutamente nada.
 * 3. En el instante exacto en que detecta una diferencia (por ejemplo, de ON a BLINK
 *    porque llegó un comando BLINK:2), actualiza modoAnterior con el nuevo valor,
 *    reinicia el contador de tiempo (ultimoCambioModo) y llama a enviarStatus(),
 *    lo cual dispara automáticamente una notificación sin que el cliente tenga que pedirla.
 */
void revisarCambioDeModo() {
  String modoActual = obtenerModoActual();

  if (modoActual != modoAnterior) {
    modoAnterior = modoActual;
    ultimoCambioModo = millis();

    Serial.print("Cambio de modo detectado: ");
    Serial.println(modoActual);

    enviarStatus();   // notifica automáticamente el cambio
  }
}


// ============================================================
// CALLBACKS BLE
// ============================================================

/*
 * CALLBACK DE ESCRITURA BLE (Modelo orientado a eventos)
 * A diferencia de la Práctica 1, donde se hacía polling constante preguntando
 * "¿ya llegó algo por Serial?", aquí el propio sistema BLE avisa automáticamente
 * cuando el cliente (celular) escribe algo en la característica.
 *
 * ¿Qué hace esta función?
 * 1. Toma el valor recibido, lo limpia con trim() y lo pasa a mayúsculas con
 *    toUpperCase(), para que el comando funcione sin importar cómo se haya escrito.
 * 2. Si el comando llega vacío, lo ignora.
 * 3. Compara el comando contra cada uno de los comandos soportados (ON, OFF, AUTO,
 *    BLINK:x, READ_POT, STATUS) y modifica las variables globales correspondientes,
 *    igual que se hacía antes desde el Monitor Serial en la Práctica 1.
 */

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

    } else if (comando == "READ_POT") {
      leyendoPot = true;
      inicioLecturaPot = millis();
      ultimaLecturaPotEnviada = 0;
      Serial.println("Iniciando READ_POT (5 segundos)...");

    } else if (comando == "STATUS") {
      enviarStatus();
    }
  }
};

/*
 * CALLBACK DE DESCONEXIÓN BLE
 * Por defecto, cuando un cliente BLE se desconecta, el ESP32 deja de anunciarse
 * y ya nadie más puede volver a conectarse sin reiniciar la placa.
 *
 * ¿Qué hace esta función?
 * 1. Detecta el momento exacto en que el cliente (celular) se desconecta.
 * 2. Vuelve a llamar a BLEDevice::startAdvertising() para que el ESP32 empiece
 *    a anunciarse otra vez y cualquier Central pueda encontrarlo y conectarse de nuevo.
 */

class ServerCallback : public BLEServerCallbacks {
  void onDisconnect(BLEServer *server) {
    Serial.println("Cliente BLE desconectado. Reiniciando advertising...");
    BLEDevice::startAdvertising();
  }
};


// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);

  BLEDevice::init("KETEIMPORTA-ESP32");
  BLEServer *esp32BleServer = BLEDevice::createServer();
  esp32BleServer->setCallbacks(new ServerCallback());

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

void loop() {
  manejarBoton();

  if (!sistemaHabilitado) {
    digitalWrite(LED_PIN, LOW);
    ledEncendido = false;
  } else if (estadoA) {
    manejarParpadeoEstadoA();
  }

  revisarCambioDeModo();

  if (leyendoPot) {
    manejarLecturaPot();
  }
}
