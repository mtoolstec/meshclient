#ifdef ARDUINO_M5STACK_CARDPUTER
/**
 * @file keyboard.cpp
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-22
 *
 * @copyright Copyright (c) 2023
 *
 */
#include "Keyboard.h"
#include <driver/gpio.h>

#include "Arduino.h"

#ifndef CARDPUTER_ADV
#define digitalWrite(pin, level) gpio_set_level((gpio_num_t)pin, level)
#define digitalRead(pin) gpio_get_level((gpio_num_t)pin)
#endif

#ifndef CARDPUTER_ADV
void Keyboard_Class::_set_output(const std::vector<int> &pinList,
                                 uint8_t output)
{
    output = output & 0B00000111;

    digitalWrite(pinList[0], (output & 0B00000001));
    digitalWrite(pinList[1], (output & 0B00000010));
    digitalWrite(pinList[2], (output & 0B00000100));
}

uint8_t Keyboard_Class::_get_input(const std::vector<int> &pinList)
{
    uint8_t buffer = 0x00;
    uint8_t pin_value = 0x00;

    for (int i = 0; i < 7; i++)
    {
        pin_value = (digitalRead(pinList[i]) == 1) ? 0x00 : 0x01;
        pin_value = pin_value << i;
        buffer = buffer | pin_value;
    }

    return buffer;
}
#endif

void Keyboard_Class::begin()
{
#ifdef CARDPUTER_ADV
    // Initialize I2C for TCA8418 on CardPuter ADV
    // SDA=GPIO12, SCL=GPIO11
    Wire.begin(12, 11);
    delay(10); // Give I2C time to stabilize
    
    Serial.println("Initializing TCA8418 keyboard for CardPuter ADV...");
    
    // Try to initialize TCA8418
    // Default I2C address is 0x34
    if (!_tca.begin(TCA8418_DEFAULT_ADDR, &Wire)) {
        Serial.println("ERROR: TCA8418 initialization failed!");
        Serial.println("  - Check I2C connections (SDA:GPIO12, SCL:GPIO11)");
        Serial.println("  - Check I2C address (default: 0x34)");
        return;
    }
    
    Serial.println("TCA8418 detected, configuring...");
    
    // Configure TCA8418 for keyboard matrix
    // CardPuter ADV has 4 rows x 14 columns physically
    // Configure as 8x7 to match TCA8418's addressing scheme
    if (!_tca.matrix(8, 7)) {
        Serial.println("ERROR: Failed to configure TCA8418 matrix");
        return;
    }
    
    // Enable interrupts for key events (optional, we use polling)
    _tca.enableInterrupt();
    
    // Flush any pending events
    while (_tca.available() > 0) {
        _tca.getEvent();
    }
    
    Serial.println("✓ TCA8418 keyboard initialized successfully");
#else
    // Original CardPuter GPIO initialization
    for (auto i : output_list)
    {
        gpio_reset_pin((gpio_num_t)i);
        gpio_set_direction((gpio_num_t)i, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode((gpio_num_t)i, GPIO_PULLUP_PULLDOWN);
        digitalWrite(i, 0);
    }

    for (auto i : input_list)
    {
        gpio_reset_pin((gpio_num_t)i);
        gpio_set_direction((gpio_num_t)i, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)i, GPIO_PULLUP_ONLY);
    }

    _set_output(output_list, 0);
#endif
}

uint8_t Keyboard_Class::getKey(Point2D_t keyCoor)
{
    return _key_value_map[keyCoor.y][keyCoor.x].value_first;
}

#ifdef CARDPUTER_ADV
void Keyboard_Class::_updateKeyListTCA()
{
    _key_list_buffer.clear();
    
    // Read all currently pressed keys from TCA8418
    // The TCA8418 maintains a FIFO of key events
    while (_tca.available() > 0) {
        keyEvent event = _tca.getEvent();
        
        // Only process key press events (ignore release for now)
        if (event.state == KEY_JUST_PRESSED || event.state == KEY_PRESSED) {
            // Convert TCA8418 row/col to our coordinate system
            // CardPuter ADV has 4 rows x 14 cols physically
            // TCA8418 is configured as 8 rows x 7 cols
            uint8_t row = event.row;
            uint8_t col = event.col;
            
            // Map TCA row/col to our logical coordinates
            // This mapping may need adjustment based on actual hardware wiring
            if (row < 4 && col < 7) {
                Point2D_t key_coordinate;
                key_coordinate.y = row;
                // Use the same X mapping as original CardPuter
                key_coordinate.x = X_map_chart[col].value;
                
                if (key_coordinate.x < 14) {
                    _key_list_buffer.push_back(key_coordinate);
                }
            }
        }
    }
}

void Keyboard_Class::updateKeyList()
{
    _updateKeyListTCA();
}
#else
void Keyboard_Class::updateKeyList()
{
    _key_list_buffer.clear();

    for (uint8_t y = 0; y < 8; y++)
    {
        _set_output(output_list, y);
        delayMicroseconds(1);
        uint8_t value = _get_input(input_list);

        if (value)
        {
            for (uint8_t x = 0; x < 7; x++)
            {
                if (value & X_map_chart[x].high)
                {
                    Point2D_t key_coordinate;
                    key_coordinate.x = X_map_chart[x].low;
                    key_coordinate.y = y;
                    if (y < 4)
                    {
                        key_coordinate.x = X_map_chart[x].value;
                        if (key_coordinate.x < 14)
                            _key_list_buffer.push_back(key_coordinate);
                    }
                }
            }
        }
    }
}
#endif

uint8_t Keyboard_Class::isPressed()
{
    return _key_list_buffer.size();
}

bool Keyboard_Class::isChange()
{
    if (_last_key_size != _key_list_buffer.size())
    {
        _last_key_size = _key_list_buffer.size();
        return true;
    }
    else
    {
        return false;
    }
}

bool Keyboard_Class::isKeyPressed(char c)
{
    if (_key_list_buffer.size())
    {
        for (const auto &i : _key_list_buffer)
        {
            if (getKey(i) == c)
                return true;
        }
    }
    return false;
}

Keyboard_Class::KeysState Keyboard_Class::keysState()
{
    KeysState _keys_state_buffer;
    _keys_state_buffer.word.clear();
    _keys_state_buffer.del = false;
    _keys_state_buffer.enter = false;
    _keys_state_buffer.ctrl = false;
    _keys_state_buffer.shift = false;
    _keys_state_buffer.opt = false;
    _keys_state_buffer.alt = false;
    _keys_state_buffer.fn = false;
    _keys_state_buffer.tab = false;
    _keys_state_buffer.gui = false;
    _keys_state_buffer.exit_key = false;

    _key_pos_print_keys.clear();
    _key_pos_hid_keys.clear();
    _key_pos_modifier_keys.clear();

    for (const auto &i : _key_list_buffer)
    {
        uint8_t key_value = getKey(i);

        if (key_value == 0x2A)
        {
            _keys_state_buffer.del = true;
            _key_pos_hid_keys.push_back(i);
        }
        else if (key_value == 0x28)
        {
            _keys_state_buffer.enter = true;
            _key_pos_hid_keys.push_back(i);
        }
        else if (key_value == 0x80)
        {
            _keys_state_buffer.ctrl = true;
            _key_pos_modifier_keys.push_back(i);
        }
        else if (key_value == 0x81)
        {
            _keys_state_buffer.shift = true;
            _key_pos_modifier_keys.push_back(i);
        }
        else if (key_value == 0x82)
        {
            _keys_state_buffer.alt = true;
            _key_pos_modifier_keys.push_back(i);
        }
        else if (key_value == 0x83)
        {
            _keys_state_buffer.opt = true;
            _key_pos_modifier_keys.push_back(i);
        }
        else if (key_value == 0xFF)
        {
            _keys_state_buffer.fn = true;
            _key_pos_modifier_keys.push_back(i);
        }
        else if (key_value != 0x00)
        {
            if (_keys_state_buffer.shift ^ _is_caps_locked)
            {
                _keys_state_buffer.word.push_back(_key_value_map[i.y][i.x].value_second);
            }
            else
            {
                _keys_state_buffer.word.push_back(_key_value_map[i.y][i.x].value_first);
            }
            _key_pos_print_keys.push_back(i);
        }
    }

    return _keys_state_buffer;
}

#endif