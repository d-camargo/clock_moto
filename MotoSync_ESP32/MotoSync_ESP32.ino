#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RtcDS3231.h>
#include <TinyGPSPlus.h>
#include <BLEDevice.h>
#include "secrets.h" // Chaves e senhas (fora do git). Copie secrets.example.h -> secrets.h

// ===== LCD CONFIG =====
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Endereço I2C 0x27, LCD 16x2 (SDA=GPIO21, SCL=GPIO22)

// ===== GPS G10A-F30 (u-blox M10) =====
// TX do GPS -> GPIO16 (RX2) | RX do GPS -> GPIO17 (TX2) | VCC 3.3V | GND
// ponytail: baud padrão do u-blox M10 é 38400; se o Serial só mostrar lixo/nada, tente 9600
const long GPS_BAUD = 38400;
HardwareSerial GPSSerial(2);
TinyGPSPlus gps;

// ===== WI-FI: roteador de casa + roteador do celular =====
// ponytail: internet via Bluetooth não é possível (ESP32/Arduino não tem BT-PAN); usa o roteador Wi-Fi do celular
WiFiMulti wifiMulti;

// ===== BLE (app MIT App Inventor) =====
// App escreve texto "lat;lon;nome" na característica abaixo. Ex: "-19.930240;-43.987770;Trab"
// Também aceita o formato copiado do Google Maps: "-19.930240, -43.987770;Trab"
#define BLE_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_DESTINO_UUID "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
// A extensão BLE do App Inventor corta cada envio em 22 caracteres, então o app manda em 3 partes:
// "A<lat>", "O<lon>", "N<nome>" — ao chegar o "N" o destino é montado e processado.
String bleRecebido = "";   // Preenchido no callback BLE, processado no loop()
String bleLat = "", bleLon = "";

class DestinoCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override { // Nada pesado aqui (roda na task do BLE)
    String v = String(c->getValue().c_str()); // Corta no \0 que a extensão do App Inventor adiciona
    if (v.startsWith("A")) bleLat = v.substring(1);
    else if (v.startsWith("O")) bleLon = v.substring(1);
    else if (v.startsWith("N")) bleRecebido = bleLat + ";" + bleLon + ";" + v.substring(1);
    else bleRecebido = v; // Mensagem inteira "lat;lon;nome"
  }
};

class ServerCallback : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { Serial.println("BLE: celular conectado"); }
  void onMtuChanged(BLEServer* s, esp_ble_gatts_cb_param_t* p) override { Serial.printf("BLE: MTU = %d\n", p->mtu.mtu); }
  void onDisconnect(BLEServer* s) override {
    Serial.println("BLE: celular desconectado");
    BLEDevice::startAdvertising(); // Volta a ficar visível para o app
  }
};

// ===== WEB SERVER CONFIG =====
WebServer server(80);

// ===== NTP CONFIG =====
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800;  // UTC-3 Brasil
const int daylightOffset_sec = 0;

// ===== RTC CONFIG (DS3231, I2C 0x68 — divide SDA/SCL com o LCD) =====
RtcDS3231<TwoWire> Rtc(Wire);

// ===== OpenWeatherMap CONFIG =====
String apiKey = OPENWEATHER_API_KEY;   // pegue no site https://openweathermap.org/
String city = "Belo Horizonte";
String country = "BR";

// ===== TomTom CONFIG (Trânsito) =====
// Cole aqui sua chave gerada no portal: https://developer.tomtom.com/
String tomtomApiKey = TOMTOM_API_KEY;
// Coordenadas Fixas de Referência
const float HOME_LAT = -19.920198; // Coordenadas da sua Casa
const float HOME_LON = -44.089905;
const float WORK_LAT = -19.930240; // Coordenadas do Trabalho (CEFET-MG Nova Suica)
const float WORK_LON = -43.987770;

// Destino Dinâmico Atual
String destLatAtual = "-19.930240";  // Inicia apontando para o CEFET
String destLonAtual = "-43.987770";
String nomeDestino = "Trab";         // Nome padrão do destino
bool destinoManual = false;          // true = destino veio do app (BLE); Casa/Trab automático pausa até chegar
bool saiuDoDestino = false;          // Só conta "chegou" depois de ter se afastado (enviar Casa estando em casa não cancela)

// ===== Variáveis Globais =====
unsigned long lastWeatherUpdate = 0;
unsigned long lastTrafficUpdate = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long confirmacaoAte = 0;  // Até quando mostrar a tela de confirmação do destino
const unsigned long TEMPO_CONFIRMACAO = 6000; // ms

float temperatura = 0;
String condicao = "N/A";

// GPS dinâmico (módulo G10A-F30 ou enviado pelo celular)
String latAtual = "";
String lonAtual = "";
bool gpsRecebido = false;
int tempoTrabalhoMinutos = 0;
String statusTransito = "N/A";

// Navegação curva a curva (vem junto da rota TomTom, atualizada a cada 30s)
struct Manobra { float lat, lon; char txt[11]; char rua[17]; };
const int MAX_MANOBRAS = 30;       // ponytail: rota longa fica truncada; o refresh de 30s traz as próximas
const int NAV_DIST_ALERTA = 500;   // m: abaixo disso a tela de navegação toma o LCD (ajuste a gosto)
const int NAV_DIST_PASSOU = 30;    // m: perto assim da manobra = já passou, vai para a próxima
Manobra manobras[MAX_MANOBRAS];
int numManobras = 0;
int manobraAtual = 0;
int distManobra = 99999;           // m até a próxima manobra

// Protótipos das funções
void atualizarClima();
void atualizarTransito();
void handleGPSUpdate();
void handleNotFound();
void escolherDestino(float latF, float lonF);
void lerGPSModulo();
void processarBLE();
void semAcento(String& s);
void atualizarDistManobra(float lat, float lon);

void setup() {
  Serial.begin(115200);
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, 16, 17); // RX=16, TX=17

  // Inicializa LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("Iniciando...");

  // Mensagem de boas-vindas
  lcd.clear();
  lcd.setCursor(3,0);
  lcd.print(" MotoSync ");
  lcd.setCursor(0,1);
  lcd.print("   Bem-vindo! ");
  delay(3000);  // mostra por 3 segundos

  // Inicializa RTC
  Rtc.Begin();
  if (!Rtc.GetIsRunning()) Rtc.SetIsRunning(true); // DS3231 novo pode vir parado

  // Conecta na rede com melhor sinal entre as cadastradas
  wifiMulti.addAP(WIFI_CASA_SSID, WIFI_CASA_SENHA);
  wifiMulti.addAP(WIFI_CELULAR_SSID, WIFI_CELULAR_SENHA); // Roteador Wi-Fi do celular

  // NTP: o ESP32 sincroniza sozinho assim que houver internet; o RTC é acertado no loop()
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  lcd.setCursor(0,1);
  lcd.print("Conectando Wi-Fi");

  int tentativas = 0;
  while (wifiMulti.run() != WL_CONNECTED && tentativas < 20) {
    delay(500);
    tentativas++;
    lcd.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi OK!");
    lcd.setCursor(0,1);
    lcd.print(WiFi.localIP());
    Serial.print("Conectado na rede: ");
    Serial.println(WiFi.SSID());
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    delay(2000);

    // Atualiza clima na inicialização
    atualizarClima();

  } else {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Sem WiFi!");
    delay(2000);
  }

  // --- Inicializa BLE (aparece como "MotoSync" no app) ---
  BLEDevice::init("MotoSync");
  BLEDevice::setMTU(185); // Padrão é 23 (só 20 bytes por envio); o app pede MTU maior ao conectar
  BLEServer* bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallback());
  BLEService* bleService = bleServer->createService(BLE_SERVICE_UUID);
  BLECharacteristic* destinoChar = bleService->createCharacteristic(
      BLE_DESTINO_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  destinoChar->setCallbacks(new DestinoCallback());
  bleService->start();
  BLEDevice::getAdvertising()->addServiceUUID(BLE_SERVICE_UUID);
  BLEDevice::startAdvertising();
  Serial.println("BLE ativo: MotoSync");

  // --- Inicializa Servidor Web ---
  server.on("/gps", HTTP_GET, handleGPSUpdate);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Servidor HTTP iniciado na porta 80.");

  // --- Inicializa DNS Local (motosync.local) ---
  if (MDNS.begin("motosync")) {
    Serial.println("MDNS ativo! Acesse via: http://motosync.local/gps");
    MDNS.addService("http", "tcp", 80);
  }
}

void loop() {
  // Mantém os servidores HTTP e mDNS rodando ativamente (Não bloqueante!)
  server.handleClient();
  lerGPSModulo();
  processarBLE();

  // Reconexão inteligente e dinâmica se perder o Wi-Fi do celular
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 5000) { // Tenta reconectar a cada 5 segundos sem travar o loop
      lastReconnectAttempt = millis();
      Serial.println("Conexao Wi-Fi perdida! Procurando roteador ou celular...");
      wifiMulti.run(); // Conecta na rede disponível (casa ou celular)
    }
  }

  // Acerta o RTC com a hora da internet assim que o NTP responder (uma vez por boot)
  static bool rtcSincronizado = false;
  time_t agora = time(nullptr);
  if (!rtcSincronizado && agora > 1700000000) { // > nov/2023 = hora NTP válida
    struct tm* t = localtime(&agora);
    Rtc.SetDateTime(RtcDateTime(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                                t->tm_hour, t->tm_min, t->tm_sec));
    rtcSincronizado = true;
    RtcDateTime lido = Rtc.GetDateTime();
    Serial.printf("RTC atualizado via NTP! %02u/%02u/%04u %02u:%02u (erro I2C=%d)\n",
                  lido.Day(), lido.Month(), lido.Year(), lido.Hour(), lido.Minute(), Rtc.LastError());
  }

  // Código não-bloqueante para gerenciar o display LCD a cada 1 segundo
  if (millis() - lastDisplayUpdate > 1000) {
    lastDisplayUpdate = millis();

    // Alterna entre 2 telas (Ciclo total de 11 segundos: 8s principal, 3s clima)
    int cicloTime = millis() % 11000;
    int telaAtual = (cicloTime < 8000) ? 0 : 1; // 0 = Principal (Relógio + Trânsito), 1 = Clima
    if (manobraAtual < numManobras && distManobra <= NAV_DIST_ALERTA) telaAtual = 2; // Navegação tem prioridade
    if (millis() < confirmacaoAte) telaAtual = 3; // Confirmação de destino novo tem prioridade sobre tudo
    static int ultimaTela = -1;

    // Só limpa a tela se ela mudar (evita flicker)
    if (telaAtual != ultimaTela) {
      lcd.clear();
      ultimaTela = telaAtual;
    }

    if (telaAtual == 0) {
      // ===== TELA 1: RELÓGIO + DATA + TRÂNSITO PARA O TRABALHO =====
      RtcDateTime now = Rtc.GetDateTime();
      char line0[17];
      char line1[17];

      // Formata a Hora (HH:MM)
      char timeStr[6];
      snprintf(timeStr, sizeof(timeStr), "%02u:%02u", now.Hour(), now.Minute());

      // Formata a Data (DD/MM)
      char dateStr[6];
      snprintf(dateStr, sizeof(dateStr), "%02u/%02u", now.Day(), now.Month());

      if (gpsRecebido) {
        char labelDest[5];
        snprintf(labelDest, sizeof(labelDest), "%-4s", nomeDestino.c_str());
        
        // Ex: "14:35   Casa:28m"
        snprintf(line0, sizeof(line0), "%-5s   %-4s:%-2dm", timeStr, labelDest, tempoTrabalhoMinutos);

        // Abrevia o status do trânsito para caber nos 5 caracteres restantes
        String transAbbrev = statusTransito;
        if (transAbbrev == "Moderado") transAbbrev = "Mod. ";
        else if (transAbbrev == "Intenso") transAbbrev = "Int. ";
        else if (transAbbrev == "Livre") transAbbrev = "Livre";
        else transAbbrev = "N/A  ";

        // Ex: "30/05   Tr:Livre"
        snprintf(line1, sizeof(line1), "%-5s   Tr:%-5s", dateStr, transAbbrev.c_str());
      } else {
        char labelDest[5];
        snprintf(labelDest, sizeof(labelDest), "%-4s", nomeDestino.c_str());
        
        // Ex: "14:35   Trab:--m"
        snprintf(line0, sizeof(line0), "%-5s   %-4s:--m", timeStr, labelDest);
        // Ex: "30/05   Sem GPS "
        snprintf(line1, sizeof(line1), "%-5s   Sem GPS ", dateStr);
      }

      lcd.setCursor(0,0);
      lcd.print(line0);
      lcd.setCursor(0,1);
      lcd.print(line1);

    } else if (telaAtual == 3) {
      // ===== TELA 4: CONFIRMAÇÃO DE DESTINO (logo após o app enviar) =====
      char line0[17];
      char line1[17];
      // Ex: "Destino: Trab   " / "21min  Tr:Livre "
      snprintf(line0, sizeof(line0), "Destino: %-7s", nomeDestino.c_str());
      if (tempoTrabalhoMinutos > 0 || statusTransito != "N/A")
        snprintf(line1, sizeof(line1), "%3dmin Tr:%-6s", tempoTrabalhoMinutos, statusTransito.c_str());
      else
        snprintf(line1, sizeof(line1), "%-16s", gpsRecebido ? "Sem internet" : "Aguardando GPS");
      lcd.setCursor(0,0);
      lcd.print(line0);
      lcd.setCursor(0,1);
      lcd.print(line1);

    } else if (telaAtual == 2) {
      // ===== TELA 3: PRÓXIMA MANOBRA =====
      char line0[17];
      char line1[17];
      // Ex: "Vire dir    250m" / "Av Joao Cesar de"
      snprintf(line0, sizeof(line0), "%-10s%5dm", manobras[manobraAtual].txt, (distManobra + 5) / 10 * 10);
      snprintf(line1, sizeof(line1), "%-16s", manobras[manobraAtual].rua);
      lcd.setCursor(0,0);
      lcd.print(line0);
      lcd.setCursor(0,1);
      lcd.print(line1);

    } else if (telaAtual == 1) {
      // ===== TELA 2: CLIMA =====
      lcd.setCursor(0,0);
      lcd.print("Temp:");
      lcd.print(temperatura, 1);
      lcd.print((char)223); // símbolo de grau
      lcd.print("C   ");

      lcd.setCursor(0,1);
      lcd.print(condicao);
      lcd.print("    ");
    }
  }

  // Atualiza clima a cada 10 minutos (Se conectado)
  if (WiFi.status() == WL_CONNECTED && millis() - lastWeatherUpdate > 600000) {
    atualizarClima();
  }

  // Atualiza trânsito a cada 30s se o GPS estiver ativo
  if (WiFi.status() == WL_CONNECTED && gpsRecebido && millis() - lastTrafficUpdate > 30000) {
    atualizarTransito();
  }
}

// --- FUNÇÃO DO SERVIDOR WEB: Recebe GPS do Celular ---
void handleGPSUpdate() {
  if (server.hasArg("lat") && server.hasArg("lon")) {
    latAtual = server.arg("lat");
    lonAtual = server.arg("lon");
    
    // Corrige vírgulas para pontos (essencial para celulares em Português-BR!)
    latAtual.replace(",", ".");
    lonAtual.replace(",", ".");
    
    escolherDestino(latAtual.toFloat(), lonAtual.toFloat());

    // Permite sobresscrever manualmente caso enviado pelo celular
    if (server.hasArg("destLat") && server.hasArg("destLon")) {
      destLatAtual = server.arg("destLat");
      destLonAtual = server.arg("destLon");
      destLatAtual.replace(",", ".");
      destLonAtual.replace(",", ".");
    }
    if (server.hasArg("nome")) {
      nomeDestino = server.arg("nome");
      if (nomeDestino.length() > 4) {
        nomeDestino = nomeDestino.substring(0, 4); // Limita a 4 caracteres para alinhar no LCD
      }
    }
    
    gpsRecebido = true;
    
    server.send(200, "text/plain", "GPS e Destino recebidos com sucesso!");
    
    Serial.print("GPS Atualizado: Lat=");
    Serial.print(latAtual);
    Serial.print(" | Lon=");
    Serial.print(lonAtual);
    Serial.print(" | Destino=");
    Serial.print(nomeDestino);
    Serial.print(" (");
    Serial.print(destLatAtual);
    Serial.print(",");
    Serial.print(destLonAtual);
    Serial.println(")");
    
    // Força a atualização do trânsito imediatamente
    atualizarTransito();
  } else {
    server.send(400, "text/plain", "Erro: Informe os parametros 'lat' e 'lon'.");
  }
}

// --- Inteligência de Rota Automática baseada na proximidade ---
void escolherDestino(float latF, float lonF) {
  float distToHome = sqrt(pow(latF - HOME_LAT, 2) + pow(lonF - HOME_LON, 2));
  float distToWork = sqrt(pow(latF - WORK_LAT, 2) + pow(lonF - WORK_LON, 2));

  const float PROXIMITY_RADIUS = 0.005; // Cerca de 500 metros

  if (destinoManual) { // Mantém o destino do app até chegar perto dele
    float distToDest = sqrt(pow(latF - destLatAtual.toFloat(), 2) + pow(lonF - destLonAtual.toFloat(), 2));
    if (distToDest >= PROXIMITY_RADIUS) saiuDoDestino = true;
    else if (saiuDoDestino) destinoManual = false; // Chegou: volta ao Casa/Trab automático
    return;
  }

  if (distToHome < PROXIMITY_RADIUS) {
    destLatAtual = String(WORK_LAT, 6);
    destLonAtual = String(WORK_LON, 6);
    nomeDestino = "Trab";
  }
  else if (distToWork < PROXIMITY_RADIUS) {
    destLatAtual = String(HOME_LAT, 6);
    destLonAtual = String(HOME_LON, 6);
    nomeDestino = "Casa";
  }
}

// --- DESTINO RECEBIDO PELO APP VIA BLE: "lat;lon;nome" ---
void processarBLE() {
  if (bleRecebido == "") return;
  String msg = bleRecebido;
  bleRecebido = "";
  Serial.println("BLE recebido: " + msg);
  msg.replace(", ", ";"); // Separador do Google Maps ("lat, lon")
  msg.replace(",", ".");  // Celular em PT-BR pode mandar vírgula decimal

  int p1 = msg.indexOf(';');
  int p2 = msg.indexOf(';', p1 + 1);
  if (p1 < 0) { Serial.println("BLE: formato invalido -> " + msg); return; }
  float lat = msg.substring(0, p1).toFloat();
  float lon = msg.substring(p1 + 1, p2 < 0 ? msg.length() : p2).toFloat();
  if (lat == 0 || lon == 0 || fabs(lat) > 90 || fabs(lon) > 180) { Serial.println("BLE: coordenada invalida -> " + msg); return; }

  destLatAtual = String(lat, 6);
  destLonAtual = String(lon, 6);
  nomeDestino = p2 < 0 ? "" : msg.substring(p2 + 1, p2 + 5); // 4 caracteres no LCD
  nomeDestino.trim();
  if (nomeDestino == "") nomeDestino = "Dest";
  destinoManual = true;
  saiuDoDestino = false;
  Serial.println("BLE destino: " + nomeDestino + " (" + destLatAtual + "," + destLonAtual + ")");

  tempoTrabalhoMinutos = 0; // Zera para a confirmação não mostrar o tempo do destino antigo
  statusTransito = "N/A";
  if (WiFi.status() == WL_CONNECTED && gpsRecebido) atualizarTransito();
  confirmacaoAte = millis() + TEMPO_CONFIRMACAO;
  lastDisplayUpdate = 0; // Redesenha o LCD já no próximo loop
}

// --- LÊ O MÓDULO GPS G10A-F30 (NMEA via UART2) ---
void lerGPSModulo() {
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  if (gps.location.isUpdated() && gps.location.isValid()) {
    latAtual = String(gps.location.lat(), 6);
    lonAtual = String(gps.location.lng(), 6);
    escolherDestino(gps.location.lat(), gps.location.lng());
    atualizarDistManobra(gps.location.lat(), gps.location.lng());

    if (!gpsRecebido) { // Primeiro fix: busca trânsito na hora; depois segue o timer de 30s
      gpsRecebido = true;
      Serial.print("GPS modulo com fix: ");
      Serial.print(latAtual);
      Serial.print(",");
      Serial.println(lonAtual);
      atualizarTransito();
    }
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "MotoSync: Rota nao encontrada.");
}

// --- BUSCA CLIMA NA API OPENWEATHERMAP ---
void atualizarClima() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;

    String cidadeURL = city;
    cidadeURL.replace(" ", "%20");
    String url = "http://api.openweathermap.org/data/2.5/weather?q=" 
                + cidadeURL + "," + country + "&appid=" + apiKey + "&units=metric&lang=pt";

    if (http.begin(client, url)) {
      int httpCode = http.GET();

      if (httpCode == 200) {
        String payload = http.getString();
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
          temperatura = doc["main"]["temp"];
          condicao = doc["weather"][0]["description"].as<String>();
          
          semAcento(condicao);
        }
        lastWeatherUpdate = millis();
      }
      http.end();
    }
  }
}

// --- Remove acentos (o LCD não tem esses caracteres) ---
void semAcento(String& s) {
  const char* de[]   = {"á","à","ã","â","é","ê","í","ó","õ","ô","ú","ç","Á","À","Ã","Â","É","Ê","Í","Ó","Õ","Ô","Ú","Ç"};
  const char* para[] = {"a","a","a","a","e","e","i","o","o","o","u","c","A","A","A","A","E","E","I","O","O","O","U","C"};
  for (int i = 0; i < 24; i++) s.replace(de[i], para[i]);
}

// --- Converte a manobra TomTom (ex: "TURN_RIGHT") em texto curto para o LCD (até 10 caracteres) ---
String textoManobra(const String& m, int saidaRotatoria) {
  if (m.startsWith("ARRIVE")) return "Chegada";
  if (m.startsWith("ROUNDABOUT")) return saidaRotatoria > 0 ? "Rot saida" + String(saidaRotatoria) : "Rotatoria";
  if (m.indexOf("UTURN") >= 0) return "Retorno";
  String lado = m.indexOf("LEFT") >= 0 ? "esq" : m.indexOf("RIGHT") >= 0 ? "dir" : "";
  if (lado == "") return "Siga";
  return (m.startsWith("TURN") || m.startsWith("SHARP") ? "Vire " : "Mant ") + lado;
}

// --- Distância até a próxima manobra; avança quando passa por ela ---
void atualizarDistManobra(float lat, float lon) {
  while (manobraAtual < numManobras) {
    float dy = (manobras[manobraAtual].lat - lat) * 110540.0;
    float dx = (manobras[manobraAtual].lon - lon) * 111320.0 * cos(lat * DEG_TO_RAD);
    distManobra = sqrt(dx * dx + dy * dy); // ponytail: distância em linha reta, não pela rua; ok para avisar a curva
    if (distManobra > NAV_DIST_PASSOU) break;
    manobraAtual++;
  }
}

// --- BUSCA ROTA E TRÂNSITO NA API TOMTOM ---
void atualizarTransito() {
  if (WiFi.status() == WL_CONNECTED && gpsRecebido) {
    WiFiClient client;
    HTTPClient http;

    // URL TomTom Routing API otimizada (retorna apenas o resumo, evitando estourar a RAM do ESP)
    String url = "http://api.tomtom.com/routing/1/calculateRoute/" 
                 + latAtual + "," + lonAtual + ":" + destLatAtual + "," + destLonAtual 
                 + "/json?key=" + tomtomApiKey + "&traffic=true&travelMode=car&routeRepresentation=summaryOnly&instructionsType=coded";

    if (http.begin(client, url)) {
      int httpCode = http.GET();

      if (httpCode == 200) {
        String payload = http.getString();
        
        // Filtra só o que usamos (resumo + manobras)
        JsonDocument filtro;
        filtro["routes"][0]["summary"]["travelTimeInSeconds"] = true;
        filtro["routes"][0]["summary"]["trafficDelayInSeconds"] = true;
        JsonObject fi = filtro["routes"][0]["guidance"]["instructions"].add<JsonObject>();
        fi["point"] = true;
        fi["maneuver"] = true;
        fi["street"] = true;
        fi["roundaboutExitNumber"] = true;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filtro));

        if (!error) {
          long tempoSegundos = doc["routes"][0]["summary"]["travelTimeInSeconds"];
          long atrasoSegundos = doc["routes"][0]["summary"]["trafficDelayInSeconds"];
          
          tempoTrabalhoMinutos = tempoSegundos / 60;
          
          // Determina o status do trânsito com base no atraso em segundos (trafficDelayInSeconds)
          if (atrasoSegundos <= 60) {
            statusTransito = "Livre";
          } else if (atrasoSegundos <= 300) {
            statusTransito = "Moderado";
          } else {
            statusTransito = "Intenso";
          }

          // Manobras: a 1ª é a partida (posição atual), então a próxima de verdade é a de índice 1
          numManobras = 0;
          for (JsonObject in : doc["routes"][0]["guidance"]["instructions"].as<JsonArray>()) {
            if (numManobras >= MAX_MANOBRAS) break;
            Manobra& mb = manobras[numManobras++];
            mb.lat = in["point"]["latitude"];
            mb.lon = in["point"]["longitude"];
            strlcpy(mb.txt, textoManobra(in["maneuver"].as<String>(), in["roundaboutExitNumber"] | 0).c_str(), sizeof(mb.txt));
            String rua = in["street"] | "";
            rua.replace("Avenida ", "Av ");
            rua.replace("Rua ", "R ");
            semAcento(rua);
            strlcpy(mb.rua, rua.c_str(), sizeof(mb.rua));
          }
          manobraAtual = numManobras > 1 ? 1 : numManobras;
          atualizarDistManobra(latAtual.toFloat(), lonAtual.toFloat());

          Serial.printf("Manobras: %d | Proxima: %s %s (%dm)\n", numManobras,
                        manobraAtual < numManobras ? manobras[manobraAtual].txt : "-",
                        manobraAtual < numManobras ? manobras[manobraAtual].rua : "", distManobra);
          Serial.print("Tempo TomTom: ");
          Serial.print(tempoTrabalhoMinutos);
          Serial.print(" min | Status: ");
          Serial.println(statusTransito);
        } else {
          Serial.print("Erro parse TomTom: ");
          Serial.println(error.c_str());
        }
        lastTrafficUpdate = millis();
      } else {
        Serial.print("Erro HTTP TomTom: ");
        Serial.println(httpCode);
      }
      http.end();
    }
  }
}
