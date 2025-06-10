#include <Wire.h>
#define USE_SERIAL_1602_LCD
#include "LCDBigNumbers.hpp"
#include <LiquidCrystal_I2C.h>
#include <DS1302.h>

// --- DEFINIÇÕES E OBJETOS GLOBAIS ---
#define LCD_I2C_ADDRESS 0x27
#define RST_PIN 8
#define DAT_PIN 7
#define CLK_PIN 6

// Objetos
LiquidCrystal_I2C myLCD(LCD_I2C_ADDRESS, 16, 2);
LCDBigNumbers bigNums(&myLCD, BIG_NUMBERS_FONT_3_COLUMN_2_ROWS_VARIANT_2);
DS1302 rtc(RST_PIN, DAT_PIN, CLK_PIN);

// Variáveis de controle de tempo
unsigned long previousMillis = 0;

// Função de saudação inteligente
String saudacao(int hora) {
  if (hora >= 6 && hora < 12) {
    return "Bom dia!";
  } else if (hora >= 12 && hora < 18) {
    return "Boa tarde!";
  } else {
    return "Boa noite!";
  }
}

void setup() {
  Wire.begin();
  myLCD.init();
  myLCD.backlight();
  myLCD.clear();
  
  bigNums.begin();

  rtc.writeProtect(false);
  rtc.halt(false);

  // --- Ajuste de hora manual (use apenas uma vez, depois comente!) ---
  // Para o dia 09/06/2025, 22:15
  //Time t_set(2025, 6, 9, 22, 41, 0, 2); 
  //rtc.time(t_set);
  
  // Mostrar saudação no startup
  Time now = rtc.time();
  String msg = saudacao(now.hr);
  myLCD.setCursor(0, 0);
  myLCD.print(msg);
  myLCD.setCursor(0, 1);
  myLCD.print("FAZER 250");
  delay(4000);
  myLCD.clear();
}

void loop() {
  // Atualiza a cada 500ms para o pisca-pisca ficar visível
  if (millis() - previousMillis >= 500) {
    previousMillis = millis();

    Time t = rtc.time();

    // Buffer para a string. 8 é um tamanho seguro para "HH: MM" + nulo
    char timeString[8];

    // --- A LÓGICA CORRETA DO EXEMPLO, ADAPTADA ---
    // A largura do meio é sempre de 2 caracteres
    if (t.sec % 2 == 1) { // Usando % 2 == 1 como no exemplo
        // Formato: Hora, DOIS-PONTOS, ESPAÇO, Minuto
        snprintf_P(timeString, sizeof(timeString), PSTR("%02d:""%02d"), t.hr, t.min);
    } else {
        // Formato: Hora, ESPAÇO, ESPAÇO, Minuto
        snprintf_P(timeString, sizeof(timeString), PSTR("%02d"ONE_COLUMN_SPACE_STRING"%02d"), t.hr, t.min);
    }

    // Usamos a função da biblioteca para posicionar o cursor
    // O valor 0 deve preencher melhor a tela 16x2
    bigNums.setBigNumberCursor(1);

    // A biblioteca desenha a string de LARGURA CONSTANTE,
    // sobreescrevendo o anterior sem deslocamento.
    bigNums.print(timeString);
  }
}
