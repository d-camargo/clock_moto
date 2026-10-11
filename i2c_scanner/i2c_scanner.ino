#include <Wire.h>

void setup() {
  // Pinos padrão da placa: ESP8266 = D2(SDA)/D1(SCL), ESP32 = GPIO21(SDA)/GPIO22(SCL)
  Wire.begin();
  
  Serial.begin(115200);
  while (!Serial);
  Serial.println("\n--- I2C Scanner de Diagnostico ---");
}

void loop() {
  byte error, address;
  int nDevices = 0;

  Serial.println("Escaneando barramento I2C...");

  for (address = 1; address < 127; address++ ) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("Dispositivo encontrado no endereco: 0x");
      if (address < 16) Serial.print("0");
      Serial.print(address, HEX);
      Serial.println(" !");
      nDevices++;
    }
    else if (error == 4) {
      Serial.print("Erro desconhecido no endereco: 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    }
  }

  if (nDevices == 0) {
    Serial.println("Nenhum dispositivo I2C encontrado.\n");
  } else {
    Serial.println("Escaneamento pronto.\n");
  }

  delay(3000); // Escaneia a cada 3 segundos
}
