#include <Wire.h>

void setup() {
  // Inicializa I2C usando as portas universais (4 = D2 para SDA, 5 = D1 para SCL)
  Wire.begin(4, 5); 
  
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
