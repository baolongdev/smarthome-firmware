#pragma once

#ifndef TOUCH_GT911_H
#define TOUCH_GT911_H

#include <Wire.h>
#include "config.h"

#define GT911_ADDR 0x5D  // Default I2C address (0x14 when INT pulled high)
#define GT911_MAX_TOUCHES 5

struct TouchPoint {
    uint8_t id;
    uint16_t x;
    uint16_t y;
    uint16_t size;
    bool valid;
};

class GT911Touch {
public:
    GT911Touch() : _addr(GT911_ADDR), _initialized(false), _touchCount(0) {
        for (int i = 0; i < GT911_MAX_TOUCHES; i++) {
            _points[i] = {0, 0, 0, 0, false};
        }
    }
    
    bool begin(TwoWire &wire = Wire) {
        _wire = &wire;
        _wire->begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
        
        // Try both addresses
        if (checkAddress(0x5D)) _addr = 0x5D;
        else if (checkAddress(0x14)) _addr = 0x14;
        else {
            Serial.println("GT911 not found on I2C bus");
            return false;
        }
        
        // Reset touch controller
        writeReg(0x8040, 0x02);  // Soft reset
        delay(10);
        writeReg(0x8040, 0x00);  // Exit reset
        delay(50);
        
        // Read config version
        uint8_t configVer = readReg(0x8047);
        Serial.printf("GT911 Config Version: 0x%02X\n", configVer);
        
        // Read product ID
        char productId[5] = {0};
        readRegs(0x8140, (uint8_t*)productId, 4);
        Serial.printf("GT911 Product ID: %s\n", productId);
        
        // Set resolution to 480x480
        writeReg(0x8048, 0xE0);  // X low byte (480 = 0x01E0)
        writeReg(0x8049, 0x01);  // X high byte
        writeReg(0x804A, 0xE0);  // Y low byte
        writeReg(0x804B, 0x01);  // Y high byte
        
        // Touch settings
        writeReg(0x8056, 0x05);  // Max touch points = 5
        writeReg(0x8057, 0x0A);  // Trigger mode
        
        // Save config
        writeReg(0x8040, 0x01);
        delay(10);
        
        _initialized = true;
        Serial.println("GT911 initialized successfully");
        return true;
    }
    
    bool read() {
        if (!_initialized) return false;
        
        // Read touch status
        uint8_t status = readReg(0x814E);
        _touchCount = status & 0x0F;
        
        if (_touchCount == 0 || _touchCount > GT911_MAX_TOUCHES) {
            clearPoints();
            writeReg(0x814E, 0x00);  // Clear buffer
            return false;
        }
        
        // Read touch points
        for (int i = 0; i < _touchCount; i++) {
            uint8_t buf[8];
            readRegs(0x814F + i * 8, buf, 8);
            
            _points[i].id = buf[0] & 0x0F;
            _points[i].x = buf[1] | (buf[2] << 8);
            _points[i].y = buf[3] | (buf[4] << 8);
            _points[i].size = buf[5] | (buf[6] << 8);
            _points[i].valid = true;
        }
        
        // Clear buffer for next read
        writeReg(0x814E, 0x00);
        
        return _touchCount > 0;
    }
    
    int getTouchCount() const { return _touchCount; }
    
    TouchPoint getPoint(int index) const {
        if (index >= 0 && index < GT911_MAX_TOUCHES) return _points[index];
        return {0, 0, 0, 0, false};
    }
    
    bool getFirstTouch(int16_t &x, int16_t &y) {
        if (_touchCount > 0 && _points[0].valid) {
            x = _points[0].x;
            y = _points[0].y;
            return true;
        }
        return false;
    }
    
    void printDebug() {
        Serial.printf("Touch points: %d\n", _touchCount);
        for (int i = 0; i < _touchCount; i++) {
            Serial.printf("  [%d] ID:%d X:%d Y:%d Size:%d\n", 
                         i, _points[i].id, _points[i].x, _points[i].y, _points[i].size);
        }
    }

private:
    TwoWire *_wire;
    uint8_t _addr;
    bool _initialized;
    int _touchCount;
    TouchPoint _points[GT911_MAX_TOUCHES];
    
    bool checkAddress(uint8_t addr) {
        _wire->beginTransmission(addr);
        return _wire->endTransmission() == 0;
    }
    
    uint8_t readReg(uint16_t reg) {
        _wire->beginTransmission(_addr);
        _wire->write(reg >> 8);
        _wire->write(reg & 0xFF);
        _wire->endTransmission(false);
        _wire->requestFrom(_addr, 1);
        return _wire->read();
    }
    
    void readRegs(uint16_t reg, uint8_t *buf, size_t len) {
        _wire->beginTransmission(_addr);
        _wire->write(reg >> 8);
        _wire->write(reg & 0xFF);
        _wire->endTransmission(false);
        _wire->requestFrom(_addr, len);
        for (size_t i = 0; i < len; i++) {
            buf[i] = _wire->read();
        }
    }
    
    void writeReg(uint16_t reg, uint8_t val) {
        _wire->beginTransmission(_addr);
        _wire->write(reg >> 8);
        _wire->write(reg & 0xFF);
        _wire->write(val);
        _wire->endTransmission();
    }
    
    void clearPoints() {
        for (int i = 0; i < GT911_MAX_TOUCHES; i++) {
            _points[i] = {0, 0, 0, 0, false};
        }
    }
};

#endif // TOUCH_GT911_H