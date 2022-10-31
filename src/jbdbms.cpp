#include <jbdbms.h>


// Debug

static void hex( const char *label, const uint8_t *data, size_t length ) {
    Serial.printf("%s:", label);
    while( length-- ) {
        Serial.printf(" %02x", *(data++));
    }
    Serial.println();
}


// Basic methods

JbdBms::JbdBms( Stream &serial, uint8_t command_delay_ms ) 
    : _serial(serial), _delay(command_delay_ms), _prev(0), _dir_pin(-1) {
}

void JbdBms::begin( int dir_pin ) {
    _dir_pin = dir_pin;
    if( _dir_pin >= 0 ) {
        pinMode(_dir_pin, OUTPUT);
        digitalWrite(_dir_pin, LOW);  // read mode (default)
    }
}

bool JbdBms::execute( request_header_t &header, uint8_t *command, uint8_t *result ) {
    uint16_t crc;
    uint8_t stop = 0x77;

    if( !prepareCmd(header, command, crc) ) {
        return false;
    }

    uint32_t remaining = _delay - (millis() - _prev);
    if( remaining <= _delay ) {
        delay(remaining);
    }

    if( _dir_pin >= 0 ) {
        digitalWrite(_dir_pin, HIGH);  // write mode
    }

    _serial.flush();  // make sure read buffer is empty
    bool rc = (_serial.write((uint8_t *)&header, sizeof(header)) == sizeof(header))
           && (_serial.write(command, header.length) == header.length)
           && (_serial.write((uint8_t *)&crc, sizeof(crc)) == sizeof(crc))
           && (_serial.write(&stop, sizeof(stop)) == sizeof(stop));
    _serial.flush();  // wait until write is done 

    if( _dir_pin >= 0 ) {
        digitalWrite(_dir_pin, LOW);  // read mode (default)
    }

    if( rc ) {
        response_header_t header = {0};
        rc = (_serial.readBytes((uint8_t *)&header, sizeof(header)) == sizeof(header))
          && (header.start == 0xdd && header.length <= 64)
          && (header.length == 0 || (result && _serial.readBytes(result, header.length) == header.length))
          && (_serial.readBytes((uint8_t *)&crc, sizeof(crc)) == sizeof(crc))
          && (_serial.readBytes(&stop, sizeof(stop)) == sizeof(stop))
          && isValid(header, result, crc)
          && header.returncode == 0;
    }

    _prev = millis();

    return rc;
}


// public Get-Commands

bool JbdBms::getStatus( Status_t &data ) {
    request_header_t header = { 0, READ, STATUS, 0 };
    bool rc = execute(header, 0, (uint8_t *)&data);
    swap(&data.voltage);
    swap((uint16_t *)&data.current);
    swap(&data.remainingCapacity);
    swap(&data.nominalCapacity);
    swap(&data.cycles);
    swap(&data.productionDate);
    swap(&data.balanceLow);
    swap(&data.balanceHigh);
    swap(&data.fault);
    // for (size_t i = 0; i < sizeof(data.temperatures)/sizeof(*data.temperatures); i++) {
    //     swap(&data.temperatures[i]);
    // }
    return rc;
}
    
bool JbdBms::getCells( Cells_t &data ) {
    request_header_t header = { 0, READ, CELLS, 0 };
    bool rc = execute(header, 0, (uint8_t *)&data);
    for (size_t i = 0; i < sizeof(data.voltages)/sizeof(*data.voltages); i++) {
        swap(&data.voltages[i]);
    }
    return rc;
}
    
bool JbdBms::getHardware( Hardware_t &data ) {
    request_header_t header = { 0, READ, HARDWARE, 0 };
    return execute(header, 0, (uint8_t *)&data);
}


// public Set-Command

bool JbdBms::setMosfetStatus( mosfet_t status ) {
    request_header_t header = { 0, WRITE, MOSFET, 2 };
    uint8_t mosfetStatus[] = { 0, status };
    return execute(header, mosfetStatus, 0);
}


// Private Stuff (used internally, not by library user)

// Calculate 16-bit crc of request
// Return crc (0 on error)
uint16_t JbdBms::genRequestCrc( request_header_t &header, uint8_t *data ) {
    return genCrc(header.command, header.length, data);
}

// Calculate 16-bit crc of response
// Return crc (0 on error)
uint16_t JbdBms::genResponseCrc( response_header_t &header, uint8_t *data ) {
    return genCrc(header.returncode, header.length, data);
}

uint16_t JbdBms::genCrc( uint8_t byte, uint8_t len, uint8_t *data ) {
    uint16_t crc = 0;

    if( len < 31 && (len == 0 || data)) {
        crc -= byte;
        crc -= len;
        while( len-- ) {
            crc -= *(data++);
        }
    }

    return swap(&crc);
}

// Check crc of result
// Return true if calculated and stored crc match 
bool JbdBms::isValid( response_header_t &header, uint8_t *data, uint16_t crc ) {
    return genResponseCrc(header, data) == crc;
}

// Set start and crc bytes of command
// Return length of command or 0 on errors
bool JbdBms::prepareCmd( request_header_t &header, uint8_t *data, uint16_t &crc ) {
    header.start = 0xdd;
    crc = genRequestCrc(header, data);
    return crc != 0;
}
