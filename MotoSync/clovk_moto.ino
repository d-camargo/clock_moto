#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

// ===== LCD CONFIG =====
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Endereço I2C 0x27, LCD 16x2



// ===== WEB SERVER CONFIG =====
ESP8266WebServer server(80);

// ===== NTP CONFIG =====
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800;  // UTC-3 Brasil
const int daylightOffset_sec = 0;

// ===== RTC CONFIG (DS1302) =====
ThreeWire myWire(12, 14, 13); // DAT (GPIO12/D6), CLK (GPIO14/D5), RST (GPIO13/D7)
RtcDS1302<ThreeWire> Rtc(myWire);

// ===== OpenWeatherMap CONFIG =====
String apiKey = "SUA_API_KEY_OPENWEATHERMAP";   // pegue no site https://openweathermap.org/
String city = "Belo Horizonte";
String country = "BR";

// ===== TomTom CONFIG (Trânsito) =====
// Cole aqui sua chave gerada no portal: https://developer.tomtom.com/
String tomtomApiKey = "SUA_API_KEY_TOMTOM"; 
// Coordenadas Fixas de Referência
const float HOME_LAT = 0.000000; // Coordenadas da sua Casa
const float HOME_LON = 0.000000;
const float WORK_LAT = -19.930240; // Coordenadas do Trabalho (CEFET-MG Nova Suica)
const float WORK_LON = -43.987770;

// Destino Dinâmico Atual
String destLatAtual = "-19.930240";  // Inicia apontando para o CEFET
String destLonAtual = "-43.987770";
String nomeDestino = "Trab";         // Nome padrão do destino

// ===== Variáveis Globais =====
unsigned long lastWeatherUpdate = 0;
unsigned long lastTrafficUpdate = 0;
unsigned long lastDisplayUpdate = 0;

float temperatura = 0;
String condicao = "N/A";

// GPS dinâmico enviado pelo celular
String latAtual = "";
String lonAtual = "";
bool gpsRecebido = false;
int tempoTrabalhoMinutos = 0;
String statusTransito = "N/A";

// Protótipos das funções
void atualizarClima();
void atualizarTransito();
void handleGPSUpdate();
void handleNotFound();

void setup() {
  Serial.begin(115200);

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

  // Configura Wi-Fi para o Roteador do Celular (SSID "CEFETMG")
  const char* ssid = "CEFETMG";
  const char* password = "SUA_SENHA_WIFI";
  WiFi.begin(ssid, password);

  lcd.setCursor(0,1);
  lcd.print("Conectando Wi-Fi");

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
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

    // Configura NTP
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    // Atualiza RTC com hora NTP
    struct tm timeinfo;
    time_t now = time(nullptr);
    int checkTime = 0;
    while (now < 8 * 3600 * 2 && checkTime < 10) { // Aguarda sincronizar com NTP
      delay(500);
      now = time(nullptr);
      checkTime++;
    }
    
    struct tm* timeinfo_ptr = localtime(&now);
    if (now > 100000) { // Se obteve hora válida
      RtcDateTime ntpTime(
        timeinfo_ptr->tm_year + 1900,
        timeinfo_ptr->tm_mon + 1,
        timeinfo_ptr->tm_mday,
        timeinfo_ptr->tm_hour,
        timeinfo_ptr->tm_min,
        timeinfo_ptr->tm_sec
      );
      Rtc.SetDateTime(ntpTime);
      Serial.println("RTC atualizado via NTP!");
    } else {
      Serial.println("Falha ao obter hora NTP!");
    }

    // Atualiza clima na inicialização
    atualizarClima();

  } else {
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Sem WiFi!");
    delay(2000);
  }

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
  MDNS.update();

  // Reconexão inteligente e dinâmica se perder o Wi-Fi do celular
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 5000) { // Tenta reconectar a cada 5 segundos sem travar o loop
      lastReconnectAttempt = millis();
      Serial.println("Conexao Wi-Fi perdida! Reconectando ao Roteador...");
      WiFi.begin("CEFETMG", "SUA_SENHA_WIFI");
    }
  }

  // Código não-bloqueante para gerenciar o display LCD a cada 1 segundo
  if (millis() - lastDisplayUpdate > 1000) {
    lastDisplayUpdate = millis();

    // Alterna entre 2 telas (Ciclo total de 11 segundos: 8s principal, 3s clima)
    int cicloTime = millis() % 11000;
    int telaAtual = (cicloTime < 8000) ? 0 : 1; // 0 = Principal (Relógio + Trânsito), 1 = Clima
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

  // Atualiza trânsito a cada 3 minutos se o GPS estiver ativo (Para não gastar requests)
  if (WiFi.status() == WL_CONNECTED && gpsRecebido && millis() - lastTrafficUpdate > 180000) {
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
    
    // Inteligência de Rota Automática baseada na proximidade
    float latF = latAtual.toFloat();
    float lonF = lonAtual.toFloat();
    
    float distToHome = sqrt(pow(latF - HOME_LAT, 2) + pow(lonF - HOME_LON, 2));
    float distToWork = sqrt(pow(latF - WORK_LAT, 2) + pow(lonF - WORK_LON, 2));
    
    const float PROXIMITY_RADIUS = 0.005; // Cerca de 500 metros
    
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
          
          // Remove acentos
          condicao.replace("á", "a");
          condicao.replace("ã", "a");
          condicao.replace("â", "a");
          condicao.replace("é", "e");
          condicao.replace("ê", "e");
          condicao.replace("í", "i");
          condicao.replace("ó", "o");
          condicao.replace("õ", "o");
          condicao.replace("ô", "o");
          condicao.replace("ú", "u");
          condicao.replace("ç", "c");
        }
        lastWeatherUpdate = millis();
      }
      http.end();
    }
  }
}

// --- BUSCA ROTA E TRÂNSITO NA API TOMTOM ---
void atualizarTransito() {
  if (WiFi.status() == WL_CONNECTED && gpsRecebido) {
    WiFiClient client;
    HTTPClient http;

    // URL TomTom Routing API otimizada (retorna apenas o resumo, evitando estourar a RAM do ESP8266)
    String url = "http://api.tomtom.com/routing/1/calculateRoute/" 
                 + latAtual + "," + lonAtual + ":" + destLatAtual + "," + destLonAtual 
                 + "/json?key=" + tomtomApiKey + "&traffic=true&travelMode=car&routeRepresentation=summaryOnly";

    if (http.begin(client, url)) {
      int httpCode = http.GET();

      if (httpCode == 200) {
        String payload = http.getString();
        
        // Aloca espaço suficiente para o JSON otimizado
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, payload);

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
