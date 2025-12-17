#ifdef ARDUINO_M5STACK_CARDPUTER
/**
 * @file keyboard.h
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-22
 *
 * @copyright Copyright (c) 2023
 *
 */
#pragma once
#include <iostream>
#include <vector>
#include "Arduino.h"
#include "hardware_config.h"

// M5Cardputer library includes Adafruit_TCA8418
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418.h>

struct Chart_t
{
    uint8_t high : 7;
    uint8_t low : 7;
    uint8_t value : 7;
};

struct Point2D_t
{
    uint8_t x;
    uint8_t y;
};

const std::vector<int> output_list = {KB_OUTPUT_PINS[0], KB_OUTPUT_PINS[1], KB_OUTPUT_PINS[2]};
const std::vector<int> input_list = {KB_INPUT_PINS[0], KB_INPUT_PINS[1], KB_INPUT_PINS[2], KB_INPUT_PINS[3], KB_INPUT_PINS[4], KB_INPUT_PINS[5], KB_INPUT_PINS[6]};

const Chart_t X_map_chart[7] = {{1, 0, 1}, {2, 2, 3}, {4, 4, 5}, {8, 6, 7}, {16, 8, 9}, {32, 10, 11}, {64, 12, 13}};

struct KeyValue_t
{
    char value_first;
    char value_second;
    char value_third;
};

const KeyValue_t _key_value_map[4][14] = {
    {{'q', 'Q', '1'},
     {'w', 'W', '2'},
     {'e', 'E', '3'},
     {'r', 'R', '4'},
     {'t', 'T', '5'},
     {'y', 'Y', '6'},
     {'u', 'U', '7'},
     {'i', 'I', '8'},
     {'o', 'O', '9'},
     {'p', 'P', '0'},
     {'[', '{', '['},
     {']', '}', ']'},
     {'\\', '|', '\\'},
     {'\0', '\0', '\0'}},

    {{'a', 'A', '*'},
     {'s', 'S', '/'},
     {'d', 'D', '+'},
     {'f', 'F', '-'},
     {'g', 'G', '='},
     {'h', 'H', '('},
     {'j', 'J', ')'},
     {'k', 'K', '\''},
     {'l', 'L', '"'},
     {';', ':', ';'},
     {'\'', '"', '\''},
     {'\0', '\0', '\0'},
     {'\0', '\0', '\0'},
     {'\0', '\0', '\0'}},

    {{0xFF, 0xFF, 0xFF}, // fn key
     {'z', 'Z', '<'},
     {'x', 'X', '>'},
     {'c', 'C', '?'},
     {'v', 'V', '!'},
     {'b', 'B', '@'},
     {'n', 'N', '#'},
     {'m', 'M', '$'},
     {',', '<', '%'},
     {'.', '>', '^'},
     {'/', '?', '&'},
     {'\0', '\0', '\0'},
     {'\0', '\0', '\0'},
     {'\0', '\0', '\0'}},

    {{'\0', '\0', '\0'},
     {0x81, 0x81, 0x81}, // shift key
     {0x80, 0x80, 0x80}, // ctrl key  
     {0x82, 0x82, 0x82}, // alt key
     {' ', ' ', ' '},
     {' ', ' ', ' '},
     {' ', ' ', ' '},
     {0x83, 0x83, 0x83}, // opt key
     {0x2A, 0x2A, 0x2A}, // backspace
     {0x28, 0x28, 0x28}, // enter
     {'\0', '\0', '\0'},
     {'\0', '\0', '\0'},
     {'\0', '\0', '\0'},
     {'\0', '\0', '\0'}}
};

class Keyboard_Class
{
public:
    struct KeysState
    {
        std::vector<uint8_t> word;
        std::vector<uint8_t> hid_keys;
        std::vector<uint8_t> modifier_keys;
        bool del : 1;
        bool enter : 1;
        bool ctrl : 1;
        bool shift : 1;
        bool opt : 1;
        bool alt : 1;
        bool fn : 1;
        bool tab : 1;
        bool gui : 1;
        bool exit_key : 1;
    };

private:
    std::vector<Point2D_t> _key_list_buffer;
    std::vector<Point2D_t> _key_pos_print_keys; // only text: eg A,B,C
    std::vector<Point2D_t> _key_pos_hid_keys;   // print key + space, enter, del
    std::vector<Point2D_t>
        _key_pos_modifier_keys; // modifier key: eg shift, ctrl, alt
    KeysState _keys_state_buffer;
    bool _is_caps_locked;
    uint8_t _last_key_size;
    bool _useTCA;

    Adafruit_TCA8418 _tca;
    void _updateKeyListTCA();
    void _set_output(const std::vector<int> &pinList, uint8_t output);
    uint8_t _get_input(const std::vector<int> &pinList);

public:
    Keyboard_Class() : _is_caps_locked(false), _last_key_size(0), _useTCA(false) {}

    void begin();
    uint8_t getKey(Point2D_t keyCoor);

    void updateKeyList();

    inline std::vector<Point2D_t> &keyList()
    {
        return _key_list_buffer;
    }

    uint8_t isPressed();
    bool isChange();
    bool isKeyPressed(char c);
    KeysState keysState();
};

#endif