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

void Keyboard_Class::_set_output(const std::vector<int> &pinList,
                                 uint8_t output)
{
    output = output & 0B00000111;

    gpio_set_level((gpio_num_t)pinList[0], (output & 0B00000001));
    gpio_set_level((gpio_num_t)pinList[1], (output & 0B00000010));
    gpio_set_level((gpio_num_t)pinList[2], (output & 0B00000100));
}

uint8_t Keyboard_Class::_get_input(const std::vector<int> &pinList)
{
    uint8_t buffer = 0x00;
    uint8_t pin_value = 0x00;

    for (int i = 0; i < 7; i++)
    {
        pin_value = (gpio_get_level((gpio_num_t)pinList[i]) == 1) ? 0x00 : 0x01;
        pin_value = pin_value << i;
        buffer = buffer | pin_value;
    }

    return buffer;
}

void Keyboard_Class::begin()
{
    // ADV uses TCA8418 over internal I2C. Base model uses GPIO matrix.
    if (isCardputerAdv()) {
        const int tcaSda = getTcaSdaPin();
        const int tcaScl = getTcaSclPin();
        const uint8_t tcaAddr = getTcaAddress();

        if (tcaSda >= 0 && tcaScl >= 0) {
            Wire.begin(tcaSda, tcaScl);
            Serial.printf("[Keyboard] ADV I2C: SDA=%d SCL=%d addr=0x%02X\n", tcaSda, tcaScl, tcaAddr);
        } else {
            Serial.println("[Keyboard] ADV I2C pins unknown; skipping TCA8418 init");
        }

        delay(50); // let keyboard power-up

        // Try default address first; some boards may use 0x35 depending on strap.
        bool ok = false;
        if (_tca.begin(tcaAddr, &Wire)) {
            ok = true;
        } else if (tcaAddr != 0x35 && _tca.begin(0x35, &Wire)) {
            ok = true;
        }

        if (ok) {
            Serial.println("TCA8418 detected, configuring...");
            if (!_tca.matrix(8, 7)) {
                Serial.println("ERROR: Failed to configure TCA8418 matrix, falling back to GPIO");
            } else {
                _tca.enableInterrupt();
                while (_tca.available() > 0) {
                    _tca.getEvent();
                }
                Serial.println("✓ TCA8418 keyboard initialized");
                _useTCA = true;
                return;
            }
        } else {
            Serial.println("TCA8418 not responding, falling back to GPIO matrix");
        }
    }

    // GPIO keyboard initialization (original CardPuter)
    for (auto i : output_list)
    {
        gpio_reset_pin((gpio_num_t)i);
        gpio_set_direction((gpio_num_t)i, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode((gpio_num_t)i, GPIO_PULLUP_PULLDOWN);
        gpio_set_level((gpio_num_t)i, 0);
    }

    for (auto i : input_list)
    {
        gpio_reset_pin((gpio_num_t)i);
        gpio_set_direction((gpio_num_t)i, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)i, GPIO_PULLUP_ONLY);
    }

    _set_output(output_list, 0);
}

uint8_t Keyboard_Class::getKey(Point2D_t keyCoor)
{
    return _key_value_map[keyCoor.y][keyCoor.x].value_first;
}

void Keyboard_Class::_updateKeyListTCA()
{
    _key_list_buffer.clear();

    // Read all currently pressed keys from TCA8418
    // The TCA8418 maintains a FIFO of key events
    while (_tca.available() > 0) {
        keyEvent event = _tca.getEvent();

        Serial.printf("[TCA8418] event raw row=%u col=%u state=%u\n", event.row, event.col, event.state);

        // Only process key press events (ignore release for now)
        if (event.state == KEY_JUST_PRESSED || event.state == KEY_PRESSED) {
            // Convert TCA8418 row/col to our coordinate system
            // CardPuter ADV has 4 rows x 14 cols physically
            // TCA8418 is configured as 8 rows x 7 cols (1-based in the driver)
            uint8_t row = event.row > 0 ? event.row - 1 : 0; // zero-base
            uint8_t col = event.col > 0 ? event.col - 1 : 0; // zero-base

            Point2D_t key_coordinate{};

            if (row < 4 && col < 7) {
                // Upper half of the matrix → odd logical columns
                key_coordinate.y = row;
                key_coordinate.x = X_map_chart[col].value;
            } else if (row < 8 && col < 7) {
                // Lower half of the matrix → even logical columns (covers extra keys like arrows)
                key_coordinate.y = row - 4;
                key_coordinate.x = X_map_chart[col].low;
            } else {
                // Outside of configured matrix; log for diagnosis
                Serial.printf("[TCA8418] Unmapped key event r=%u c=%u state=%u\n", row, col, event.state);
                continue;
            }

            if (key_coordinate.y < 4 && key_coordinate.x < 14) {
                _key_list_buffer.push_back(key_coordinate);
            } else {
                // Keep a breadcrumb if a key lands outside our logical map
                Serial.printf("[TCA8418] Key outside map r=%u c=%u -> y=%u x=%u\n",
                              row, col, key_coordinate.y, key_coordinate.x);
            }
        }
    }
}

void Keyboard_Class::updateKeyList()
{
    if (_useTCA) {
        _updateKeyListTCA();
        return;
    }

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

        // On standard CardPuter (GPIO keyboard), some arrow keys are wired to punctuation.
        // Remap ';' to Arrow Up (0x52) and '.' to Arrow Down (0x51) for UI navigation.
        if (!isCardputerAdv()) {
            if (key_value == ';') {
                _keys_state_buffer.hid_keys.push_back(0x52); // Arrow Up
                // Do not treat as printable char
                continue;
            } else if (key_value == '.') {
                _keys_state_buffer.hid_keys.push_back(0x51); // Arrow Down
                // Do not treat as printable char
                continue;
            }
        }

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