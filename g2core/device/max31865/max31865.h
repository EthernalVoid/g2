/*
 * max31865/max31865.h - suppport for talking to the MAX31865 RTD (PT100) sensor amp/ADC
 * This file is part of the G2 project
 *
 * Copyright (c) 2017 Alden S. Hart, Jr.
 * Copyright (c) 2017 Robert Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

// Many thanks to Adafruit!
// More specifically, for their driver at
//   https://github.com/adafruit/Adafruit_MAX31865
// and their breakout board at https://adafru.it/3328

#ifndef max31865_h
#define max31865_h

#include "MotateSPI.h"
#include "MotateBuffer.h"
#include "MotateUtilities.h" // for to/fromLittle/BigEndian

using Motate::OutputPin;
using Motate::kStartHigh;
using Motate::SPIMessage;
using Motate::SPIInterrupt;
using Motate::SPIDeviceMode;
using Motate::fromBigEndian;
using Motate::toBigEndian;

// Complete class for MAX31865 drivers.
template <typename device_t>
struct MAX31865 final {
    // SPI and message handling properties
    device_t _device;
    SPIMessage _message;

    // Record if we're transmitting to prevent altering the buffers while they
    // are being transmitted still.
    volatile bool _transmitting = false;

    // We don't want to transmit until we're inited
    bool _inited = false;

    // Record what register we just requested, so we know what register the
    // the response is for (and to read the response.)
    int16_t _active_register = -1;

    // Timer to keep track of when we need to do another periodic update
    Motate::Timeout _check_timer;

    // Constructor - this is the only time we directly use the SBIBus
    template <typename SPIBus_t, typename chipSelect_t>
    MAX31865(SPIBus_t &spi_bus,
             const chipSelect_t &_cs,
             bool is_three_pin = false,
             bool fifty_hz = true
             )
    :
        _device{spi_bus.getDevice(_cs,
                                  5000000,
                                  SPIDeviceMode::kSPIMode2 | SPIDeviceMode::kSPI8Bit,
                                  400, // min_between_cs_delay_ns
                                  400, // cs_to_sck_delay_ns
                                  80   // between_word_delay_ns
                              )}
    {
        init(is_three_pin, fifty_hz);
    };

    template <typename SPIBus_t, typename chipSelect_t>
    MAX31865(std::function<void(void)> &&_interrupt,
             SPIBus_t &spi_bus,
             const chipSelect_t &_cs,
             bool is_three_pin = false,
             bool fifty_hz = true
             )
    :
        _device{spi_bus.getDevice(_cs,
                              5000000,
                              SPIDeviceMode::kSPIMode2 | SPIDeviceMode::kSPI8Bit,
                              400, // min_between_cs_delay_ns
                              400, // cs_to_sck_delay_ns
                              80   // between_word_delay_ns
        )},
        _interrupt_handler{std::move(_interrupt)}
    {
        init(is_three_pin, fifty_hz);
    };

    // Prevent copying, and prevent moving (so we know if it happens)
    MAX31865(const MAX31865 &) = delete;
    MAX31865(MAX31865 &&other) : _device{std::move(other._device)} {};


    // ###########
    // From here on we store actual values from the MAX31865, and marshall data
    // from the in_buffer buffer to them, or from the values to the out_buffer.

    // Note that this includes _startNextReadWrite() and _doneReadingCallback(),
    // which are what calls the functions to put data into the out_buffer and
    // read data from the in_buffer, respectively.

    // Also, _init() is last, so it can setup a newly created MAX31865 object.

    enum {
        INITING,
        SETUP_WIRES,
        CLEAR_FAULT,
        SETUP_BIAS,
        NEEDS_SAMPLED,
        WAITING_FOR_SAMPLE
    } _state;

    enum {
        CONFIG_reg            = 0x00,
        RTD_reg               = 0x01,
        HFAULT_reg            = 0x03,
        LFAULT_reg            = 0x05,
        FAULTSTAT_reg         = 0x07,
    };

    // IMPORTANT NOTE: The endianness of the ARM is little endian, but other processors
    //  may be different.

    uint8_t _scribble_buffer[4];

    struct {
        uint8_t address;
        union {
            uint8_t value;
            struct {
                uint8_t  fifty_or_sixty   : 1; // 0
                uint8_t  clear_fault      : 1; // 1
                uint8_t  fault_detection  : 2; // 3-2
                uint8_t  three_wire       : 1; // 4
                uint8_t  one_shot         : 1; // 5
                uint8_t  auto_mode        : 1; // 6
                uint8_t  v_bias           : 1; // 7
            } __attribute__ ((packed));
        };
    } _config;
    bool _config_needs_read = false;
    bool _config_needs_written = false;
    void _postReadConf() {
        if (WAITING_FOR_SAMPLE == _state) {
            if (!_config.one_shot) {
                _rtd_value_needs_read = true;
            } else {
//                _config_needs_read = true;
            }
        }
    };

    struct {
        uint8_t address;
        uint8_t high;
        uint8_t low;
    } _rtd_value_raw;
    float _rtd_value = -1;
    bool _rtd_value_needs_read = false;
    void _postReadRTD() {
        bool fault_detected = _rtd_value_raw.low & 0x01;
        uint16_t rtd_value_int = (_rtd_value_raw.high << 7) | (_rtd_value_raw.low >> 1);
        _rtd_value = (float)rtd_value_int / 32768.0;
        if (fault_detected) {
            _fault_status_needs_read = true;
        }
        if (_interrupt_handler) {
            _interrupt_handler();
        }
        _state = NEEDS_SAMPLED;
    };

    struct {
        uint8_t address;
        uint8_t high;
        uint8_t low;
    } _fault_high;
    bool _fault_high_needs_read = false;
    bool _fault_high_needs_written = false;
    void _postReadFaultHigh() {};

    struct {
        uint8_t address;
        uint8_t high;
        uint8_t low;
    } _fault_low;
    bool _fault_low_needs_read = false;
    bool _fault_low_needs_written = false;
    void _postReadFaultLow() {};

    struct {
        uint8_t address;
        union {
            volatile uint8_t value;
            struct {
                uint8_t  _unused            : 2; // 1-0
                uint8_t  over_under_voltage : 1; // 2
                uint8_t  open_detection     : 1; // 3
                uint8_t  ref_too_low        : 1; // 4
                uint8_t  ref_too_high       : 1; // 5
                uint8_t  under_threhold     : 1; // 6
                uint8_t  over_threshold     : 1; // 7
            } __attribute__ ((packed));
        };
    } _fault_status;
    bool _fault_status_needs_read = false;
    void _postReadFaultStatus() {
        /* here is where we would call alarm or something!! */
    };

    void _startNextReadWrite()
    {
        if (_transmitting || !_inited) { return; }
        _transmitting = true; // preemptively say we're transmitting .. as a mutex

        // We request the next register, and keep track of how long it is.
        uint8_t next_reg = 0;
        uint8_t *data_buffer = nullptr;
        int8_t register_size;

        // We write before we read -- so we don't lose what we set in the registers when writing

        // check if we need to write registers
        if (_config_needs_written)      { next_reg = 0x80 | CONFIG_reg;  data_buffer = (uint8_t*)&_config;     register_size = 1;  _config_needs_written = false;     } else
        if (_fault_high_needs_written)  { next_reg = 0x80 | HFAULT_reg;  data_buffer = (uint8_t*)&_fault_high; register_size = 2;  _fault_high_needs_written = false; } else
        if (_fault_low_needs_written)   { next_reg = 0x80 | LFAULT_reg;  data_buffer = (uint8_t*)&_fault_low;  register_size = 2;  _fault_low_needs_written = false;  } else

        // check if we need to read reagisters
        if (_config_needs_read)        { next_reg = CONFIG_reg;     data_buffer = (uint8_t*)&_config;        register_size = 1;  _config_needs_read = false;         } else
        if (_rtd_value_needs_read)     { next_reg = RTD_reg;        data_buffer = (uint8_t*)&_rtd_value_raw; register_size = 2;  _rtd_value_needs_read = false;      } else
        if (_fault_high_needs_read)    { next_reg = HFAULT_reg;     data_buffer = (uint8_t*)&_fault_high;    register_size = 2;  _fault_high_needs_read = false;     } else
        if (_fault_low_needs_read)     { next_reg = LFAULT_reg;     data_buffer = (uint8_t*)&_fault_low;     register_size = 2;  _fault_low_needs_read = false;      } else
        if (_fault_status_needs_read)  { next_reg = FAULTSTAT_reg;  data_buffer = (uint8_t*)&_fault_status;  register_size = 1;  _fault_status_needs_read = false;   } else

        // otherwise we're done here
        {
            _active_register = -1;
            _transmitting = false; // we're not really transmitting.
            return;
        }

        _active_register = next_reg;
        *data_buffer = next_reg;

        if (next_reg & 0x80) {
            // writing, copy what we're
            _scribble_buffer[0] = data_buffer[0];
            _scribble_buffer[1] = 0xFF;
            _scribble_buffer[2] = 0xFF;
            _scribble_buffer[3] = 0xFF;

            _message.setup(data_buffer, _scribble_buffer, 1+register_size, SPIMessage::DeassertAfter, SPIMessage::EndTransaction);
        } else {
            _scribble_buffer[0] = data_buffer[0];
            _scribble_buffer[1] = 0xFF;
            _scribble_buffer[2] = 0xFF;
            _scribble_buffer[3] = 0xFF;

            _message.setup(_scribble_buffer, data_buffer, 1+register_size, SPIMessage::DeassertAfter, SPIMessage::EndTransaction);
        }
        _device.queueMessage(&_message);
    };

    void _doneReadingCallback()
    {
        _transmitting = false;

        // Check to make sure it was a read, and handle it accordingly
        if (0 == (_active_register & 0x80)) {
            switch (_active_register) {
                case CONFIG_reg:     _postReadConf(); break;
                case RTD_reg:        _postReadRTD(); break;
                case HFAULT_reg:     _postReadFaultHigh(); break;
                case LFAULT_reg:     _postReadFaultLow(); break;
                case FAULTSTAT_reg:  _postReadFaultStatus(); break;

                default:
                    break;
            }
        }

        _active_register = -1;
        _startNextReadWrite();
    };

    void init(bool is_three_pin = false, bool fifty_hz = false)
    {
        _message.message_done_callback = [&] { this->_doneReadingCallback(); };

        // Establish default values, and then prepare to read the registers we can to establish starting values

        _config.v_bias = false;             // effectively enable the device
        _config.auto_mode = false;          // automatically run conversions
        _config.one_shot = false;          // this is a command
        _config.fault_detection = 0;       // this is a command - run automatic fault detection
        _config.clear_fault = false;        // this is a command, and we want to execture it
        _config.fifty_or_sixty = fifty_hz; // this is a command
        _config_needs_written = false;
        //_config_needs_read = true;

        _fault_high = {HFAULT_reg, 0xff, 0xff}; //_fault_high_needs_read = true;
        _fault_low = {LFAULT_reg, 0x00, 0x00};  //_fault_low_needs_read = true;

        _inited = true;
        //_startNextReadWrite();
        _check_timer.set(fifty_hz ? 1000/50 : 1000/60);
    };

    // interface to make this a drop-in replacement (after init) for an ADCPin

    float _vref = 3.3;
    std::function<void(void)> _interrupt_handler;

    void startSampling()
    {
        if (_check_timer.isPast()) {
            if (INITING == _state) {
                _config_needs_read = true;
                _fault_high_needs_read = true;
                _fault_low_needs_read = true;

                _check_timer.set(1);
                _startNextReadWrite();
                _state = SETUP_WIRES;
            }
            else if (SETUP_WIRES == _state) {
                _config.three_wire = false;
                _config_needs_written = true;
                _config_needs_read = true;

                _check_timer.set(10);
                _startNextReadWrite();
                _state = CLEAR_FAULT;
            }
            else if (CLEAR_FAULT == _state) {
                _config.clear_fault = true;
                _config_needs_written = true;
                _config_needs_read = true;

                _check_timer.set(1);
                _startNextReadWrite();
                _state = SETUP_BIAS;
            }
            else if (SETUP_BIAS == _state) {
                _config.v_bias = true;
                _config_needs_written = true;
                _config_needs_read = true;

                _check_timer.set(10);
                _startNextReadWrite();
                _state = NEEDS_SAMPLED;
            }
            else if (NEEDS_SAMPLED == _state) {
                _config.one_shot = true;
                _config_needs_written = true;
                _config_needs_read = true;

                _check_timer.set(1);
                _startNextReadWrite();
                _state = WAITING_FOR_SAMPLE;
            }
            else if (WAITING_FOR_SAMPLE == _state) {
                _config_needs_read = true;
                _check_timer.set(1);
                _startNextReadWrite();
            }
        }
    };

    int32_t getRaw() {
        uint16_t rtd_value_int = (_rtd_value_raw.high << 7) | (_rtd_value_raw.low >> 1);
        return rtd_value_int;
    };
    int32_t getValue() {
        return _rtd_value;
    };
    int32_t getBottom() {
        return 0;
    };
    float getBottomVoltage() {
        return 0;
    };
    int32_t getTop() {
        return 32767;
    };
    float getTopVoltage() {
        return _vref;
    };

    void setVoltageRange(const float vref,
                         const float min_expected = 0,
                         const float max_expected = -1,
                         const float ideal_steps = 1)
    {
        _vref = vref;

        // uint16_t min_expected_int = (uint16_t)((min_expected/vref) * 32767.0) << 1;
        // fault_low.high = (min_expected_int >> 8) & 0xff;
        // fault_low.low = min_expected_int & 0xff;
        // fault_low_needs_written = true;
        //
        // if (max_expected > 0) {
        //     uint16_t max_expected_int = (uint16_t)((max_expected/vref) * 32767.0) << 1;
        //     fault_high.high = (max_expected_int >> 8) & 0xff;
        //     fault_high.low = max_expected_int & 0xff;
        //     fault_high_needs_written = true;
        // }

        // differential should always be false
        // we can't control the resolution, so ignore ideal_steps too
    };
    float getVoltage() {
        return _rtd_value * _vref;
    };
    operator float() { return getVoltage(); };

    void setInterrupts(const uint32_t interrupts) {
        // ignore this -- it's too dangerous to accidentally change the SPI interrupts
    };

    // We can only support interrupt inferface option 2: a function with a closure or function pointer
    void setInterruptHandler(std::function<void(void)> &&handler) {
        _interrupt_handler = std::move(handler);
    };
    void setInterruptHandler(const std::function<void(void)> &handler) {
        _interrupt_handler = handler;
    };

};

#endif // max31865_h
