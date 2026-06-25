#include <Arduino.h>
#include "FreeRTOS.h"
#include "task.h"

extern "C" {
  #include "PinNames.h"
  #include "gpio_api.h"
}

// PA_13 is definitely our Clock. 
// We will blast data onto the remaining 3 pins simultaneously to find SDA2!
#define PIN_SCL    PA_13
#define PIN_SDA_A  PA_12
#define PIN_SDA_B  PA_14
#define PIN_SDA_C  PA_15

gpio_t scl_obj;
gpio_t sda_a;
gpio_t sda_b;
gpio_t sda_c;

// Standard I2C Start
void i2c_start() {
    gpio_write(&sda_a, 1); gpio_write(&sda_b, 1); gpio_write(&sda_c, 1);
    gpio_write(&scl_obj, 1);
    delayMicroseconds(2);
    
    gpio_write(&sda_a, 0); gpio_write(&sda_b, 0); gpio_write(&sda_c, 0);
    delayMicroseconds(2);
    
    gpio_write(&scl_obj, 0);
    delayMicroseconds(2);
}

// Standard I2C Stop
void i2c_stop() {
    gpio_write(&sda_a, 0); gpio_write(&sda_b, 0); gpio_write(&sda_c, 0);
    gpio_write(&scl_obj, 1);
    delayMicroseconds(2);
    
    gpio_write(&sda_a, 1); gpio_write(&sda_b, 1); gpio_write(&sda_c, 1);
    delayMicroseconds(2);
}

// Writes 1 byte + 1 ACK clock
void i2c_write_byte(uint8_t data) {
    // Send 8 bits (MSB first)
    for (int i = 7; i >= 0; i--) {
        uint8_t bit_val = (data >> i) & 0x01;
        gpio_write(&sda_a, bit_val);
        gpio_write(&sda_b, bit_val);
        gpio_write(&sda_c, bit_val);
        delayMicroseconds(1);
        
        gpio_write(&scl_obj, 1);
        delayMicroseconds(2);
        gpio_write(&scl_obj, 0);
        delayMicroseconds(1);
    }
    
    // 9th bit: ACK clock
    gpio_write(&sda_a, 1); gpio_write(&sda_b, 1); gpio_write(&sda_c, 1);
    delayMicroseconds(1);
    
    gpio_write(&scl_obj, 1);
    delayMicroseconds(2);
    gpio_write(&scl_obj, 0);
    delayMicroseconds(1);
}

void display_test_task(void *pvParameters) {
    gpio_init(&scl_obj, PIN_SCL);
    gpio_dir(&scl_obj, PIN_OUTPUT);
    gpio_mode(&scl_obj, PullNone);

    gpio_init(&sda_a, PIN_SDA_A);
    gpio_dir(&sda_a, PIN_OUTPUT);
    gpio_mode(&sda_a, PullNone);

    gpio_init(&sda_b, PIN_SDA_B);
    gpio_dir(&sda_b, PIN_OUTPUT);
    gpio_mode(&sda_b, PullNone);

    gpio_init(&sda_c, PIN_SDA_C);
    gpio_dir(&sda_c, PIN_OUTPUT);
    gpio_mode(&sda_c, PullNone);

    // Initial idle state
    gpio_write(&scl_obj, 0);
    gpio_write(&sda_a, 1);
    gpio_write(&sda_b, 1);
    gpio_write(&sda_c, 1);

    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("Starting WS1625 / TM-style Unified Display Driver...");

    // 1. Initialize Display Control (ON, Max Brightness)
    i2c_start();
    i2c_write_byte(0x8F);
    i2c_stop();

    while (1) {
        Serial.println("Display: All LEDs ON");
        for (int i = 0; i < 24; i++) {
            i2c_start();
            i2c_write_byte(0xC0 + i);
            i2c_write_byte(0xFF);
            i2c_stop();
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));

        Serial.println("Display: All LEDs OFF");
        for (int i = 0; i < 24; i++) {
            i2c_start();
            i2c_write_byte(0xC0 + i);  
            i2c_write_byte(0x00);
            i2c_stop();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void setup() {
    Serial.begin(115200);
    xTaskCreate(display_test_task, "DisplayTest", 1024, NULL, 1, NULL);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
