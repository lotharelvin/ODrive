/*
* The ASCII protocol is a simpler, human readable alternative to the main native
* protocol.
* In the future this protocol might be extended to support selected GCode commands.
* For a list of supported commands see doc/ascii-protocol.md
*/

/* Includes ------------------------------------------------------------------*/

#include "odrive_main.h"
#include "../build/version.h" // autogenerated based on Git state
#include "communication.h"
#include "ascii_protocol.hpp"
#include <utils.h>
#include <fibre/cpp_utils.hpp>
#include "controller.hpp"
/* Private macros ------------------------------------------------------------*/
/* Private typedef -----------------------------------------------------------*/
/* Global constant data ------------------------------------------------------*/
/* Global variables ----------------------------------------------------------*/
/* Private constant data -----------------------------------------------------*/

#define MAX_LINE_LENGTH 128
#define TO_STR_INNER(s) #s
#define TO_STR(s) TO_STR_INNER(s)

/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Function implementations --------------------------------------------------*/

// @brief Sends a line on the specified output.
template<typename ... TArgs>
void respond(StreamSink& output, bool include_checksum, const char * fmt, TArgs&& ... args) {
    char response[64];
    size_t len = snprintf(response, sizeof(response), fmt, std::forward<TArgs>(args)...);

    static uint8_t start_byte = 1;
    static uint8_t len_byte = 0;
    output.process_bytes((uint8_t*) &start_byte, 1, nullptr); // start byte
    output.process_bytes((uint8_t*) &len_byte,   1, nullptr); // byte indicating newline termination
    output.process_bytes((uint8_t*)response, len, nullptr); // TODO: use process_all instead
    if (include_checksum) {
        uint8_t checksum = 0;
        for (size_t i = 0; i < len; ++i)
            checksum ^= response[i];
        len = snprintf(response, sizeof(response), "*%u", checksum);
        output.process_bytes((uint8_t*)response, len, nullptr);
    }
    output.process_bytes((const uint8_t*)"\r\n", 2, nullptr);
}

float constrain(float in, float min, float max) {
    if(in > max) {
        return max;
    } else if(in < min) {
        return min;
    } else {
        return in;
    }
}

/**
* Parses a current set point message and sets the set points
* Assumes the message is in format "C<short1><short2><checksum>\n"

* @param msg   String: Message to parse
* @param len   int: Number of bytes in the char array
* @param i0    float&: Output parameter for axis0 set point
* @param i1    float&: Output parameter for axis1 set point
* @return      int:    1 if success, -1 if failed to find get full message or checksum failed
*/
int parse_dual_current(char* msg, int len, float& i0, float& i1) {
    const float MULTIPLIER = 100.0f;
    // Message: 1 byte for 'C', 4 bytes for values, 1 byte for checksum = 6 total bytes
    if (len != 6) {
        return -1; // error in message length
    } else {
        // get the short values from the byte array
        // NOTE: the 1st short starts at index 1! This is because the first character is "C"
        uint16_t i0_16 = (msg[2] << 8) | msg[1];
        uint16_t i1_16 = (msg[4] << 8) | msg[3];
        uint8_t rcvdCheckSum = msg[5];

        // compute checksum, including the character C
        uint8_t checkSum = 0;
        checkSum ^= msg[0]; // character 'C'
        checkSum ^= msg[1];
        checkSum ^= msg[2];
        checkSum ^= msg[3];
        checkSum ^= msg[4];

        // check if the check sum matched
        if (checkSum == rcvdCheckSum) {
            // convert to float
            i0 = (float) ((int16_t) i0_16) / MULTIPLIER;
            i1 = (float) ((int16_t) i1_16) / MULTIPLIER;
            return 1;
        } else {
            return -1;
        }
    }
    return 1;
}

/**
* Parses a command for coupled position control
* Assumes the message is in format "S<short1><short2>...<short12><checksum>\n"

* @param msg        String: Message to parse
* @param len        int: Number of bytes in the char array
* @param sp_theta   float&: Output parameter for theta position set point
* @param kp_theta   float&: Output parameter for theta position gain
* @param kd_theta   float&: Output parameter for theta derivative gain
* @param sp_gamma   float&: Output parameter for gamma position set point
* @param kp_gamma   float&: Output parameter for gamma position gain
* @param kp_gamma   float&: Output parameter for gamma derivative gain

* @return      int:    1 if success, -1 if failed to find get full message or checksum failed
*/
int parse_coupled_command(char* msg, int len,
                          float& sp_theta, float& kp_theta, float& kd_theta,
                          float& sp_gamma, float& kp_gamma, float& kd_gamma) {
    // Set multipliers:
    const float POS_MULTIPLIER = 1000.0f;
    // ^ gives 1 encoder count precision in commanding set points. Receivable range is -32.767 to 32.767 radians.
    const float GAIN_MULTIPLIER = 100.0f;
    // ^ gives 0.01 precision in setting gains. Receivable range is -327.67 to 327.67.

    // Message: 1 byte for 'S', 12 bytes for values, 1 byte for checksum = 14 total bytes
    if (len != 14) {
        return -1; // error in message length
    } else {
        // extract the short values from the byte array
        uint16_t sp_theta_16 = (msg[2] << 8) | msg[1];
        uint16_t kp_theta_16 = (msg[4] << 8) | msg[3];
        uint16_t kd_theta_16 = (msg[6] << 8) | msg[5];
        uint16_t sp_gamma_16 = (msg[8] << 8) | msg[7];
        uint16_t kp_gamma_16 = (msg[10] << 8) | msg[9];
        uint16_t kd_gamma_16 = (msg[12] << 8) | msg[11];
        uint8_t rcvdCheckSum = msg[13];

        // compute checksum, including the 'S'
        uint8_t checkSum = 0;
        for(int i = 0; i < len-1; i++) {
            checkSum ^= msg[i];
        }

        // check if the computed check sum matches the received checksum
        if (checkSum == rcvdCheckSum) {
            // convert to float
            sp_theta = (float)((int16_t)(sp_theta_16) / POS_MULTIPLIER);
            kp_theta = (float)((int16_t)(kp_theta_16) / GAIN_MULTIPLIER);
            kd_theta = (float)((int16_t)(kd_theta_16) / GAIN_MULTIPLIER);
            sp_gamma = (float)((int16_t)(sp_gamma_16) / POS_MULTIPLIER);
            kp_gamma = (float)((int16_t)(kp_gamma_16) / GAIN_MULTIPLIER);
            kd_gamma = (float)((int16_t)(kd_gamma_16) / GAIN_MULTIPLIER);
            return 1;
        } else {
            return -1;
        }
    }
    return 1;
}

void send_motor_positions(StreamSink& response_channel) {
    /***** Send encoder readings *****/
    // Sending a current control command triggers the odrive
    // to send back encoder positions
    // The message is in the form: "P<short1><short2><checksum>\n"

    // Cast the positions in counts as 2-byte shorts, it's ok to chop
    // the decimal part of the position off since we only have accuracy
    // to 1 count anyways
    float m0_fl = axes[0]->encoder_.pos_estimate_;
    float m1_fl = axes[1]->encoder_.pos_estimate_;

    //motor angles in radians... reallly shows angle of each upper leg relative to horizontal
    float alpha = axes[0]->controller_.encoder_to_rad(m0_fl) + M_PI/2.0f;
    float beta = axes[1]->controller_.encoder_to_rad(m1_fl) - M_PI/2.0f;

    // Constrain the alpha and beta angles to +- 30 radians (+- 5 rotations)
    // NOTE: Think about the consequences of sending inaccurate angles once the limits are hit
    alpha = constrain(alpha, -30.0f, 30.0f);
    beta = constrain(beta, -30.0f, 30.0f);

    float MULTIPLIER = 1000.0f;

    int16_t gamma_16 = (int16_t) ((alpha/2.0f - beta/2.0f) * MULTIPLIER);
    int16_t theta_16 = (int16_t) ((alpha/2.0f + beta/2.0f) * MULTIPLIER);
    //m0_fl and m1_fl are motor counts... but from where do they start?
    //1. motor count = 1/3 leg motor count
    //int16_t m0_16 = encoder_to(int16_t) m0_fl;
    //int16_t m1_16 = (int16_t) m1_fl;

    // compute xor checksum (whoa look at me go)
    uint8_t check_sum = 'P';
    check_sum ^= (theta_16) & 0xFF;
    check_sum ^= (theta_16 >> 8) & 0xFF;
    check_sum ^= (gamma_16) & 0xFF;
    check_sum ^= (gamma_16 >> 8) & 0xFF;

    static uint8_t start_byte = 1;
    static uint8_t len_byte = 6;
    response_channel.process_bytes((uint8_t*) &start_byte,   1, nullptr);
    response_channel.process_bytes((uint8_t*) &len_byte,     1, nullptr);
    response_channel.process_bytes((uint8_t*) "P",           1, nullptr);
    response_channel.process_bytes((uint8_t*) &theta_16,        2, nullptr);
    response_channel.process_bytes((uint8_t*) &gamma_16,        2, nullptr);
    response_channel.process_bytes((uint8_t*) &check_sum,    1, nullptr);
}


// @brief Executes an ASCII protocol command
// @param buffer buffer of ASCII encoded characters
// @param len size of the buffer
void ASCII_protocol_process_line(const uint8_t* buffer, size_t len, StreamSink& response_channel) {
    static_assert(sizeof(char) == sizeof(uint8_t));

    bool use_checksum = false;
    // copy everything into a local buffer so we can insert null-termination
    char cmd[MAX_LINE_LENGTH + 1];
    if (len > MAX_LINE_LENGTH) len = MAX_LINE_LENGTH;
    memcpy(cmd, buffer, len);
    cmd[len] = 0; // null-terminate

    // check incoming packet type
    if (cmd[0] == 'p') { // position control
        unsigned motor_number;
        float pos_setpoint, vel_feed_forward, current_feed_forward;
        int numscan = sscanf(cmd, "p %u %f %f %f", &motor_number, &pos_setpoint, &vel_feed_forward, &current_feed_forward);
        if (numscan < 2) {
            respond(response_channel, use_checksum, "invalid command format");
        } else if (motor_number >= AXIS_COUNT) {
            respond(response_channel, use_checksum, "invalid motor %u", motor_number);
        } else {
            if (numscan < 3)
                vel_feed_forward = 0.0f;
            if (numscan < 4)
                current_feed_forward = 0.0f;
            Axis* axis = axes[motor_number];
            axis->controller_.set_pos_setpoint(pos_setpoint, vel_feed_forward, current_feed_forward);
            axis->watchdog_feed();
        }

    } else if (cmd[0] == 'q') { // position control with limits
        unsigned motor_number;
        float pos_setpoint, vel_limit, current_lim;
        int numscan = sscanf(cmd, "q %u %f %f %f", &motor_number, &pos_setpoint, &vel_limit, &current_lim);
        if (numscan < 2) {
            respond(response_channel, use_checksum, "invalid command format");
        } else if (motor_number >= AXIS_COUNT) {
            respond(response_channel, use_checksum, "invalid motor %u", motor_number);
        } else {
            Axis* axis = axes[motor_number];
            axis->controller_.pos_setpoint_ = pos_setpoint;
            if (numscan >= 3)
                axis->controller_.config_.vel_limit = vel_limit;
            if (numscan >= 4)
                axis->motor_.config_.current_lim = current_lim;

            axis->watchdog_feed();
        }

    } else if (cmd[0] == 'v') { // velocity control
        unsigned motor_number;
        float vel_setpoint, current_feed_forward;
        int numscan = sscanf(cmd, "v %u %f %f", &motor_number, &vel_setpoint, &current_feed_forward);
        if (numscan < 2) {
            respond(response_channel, use_checksum, "invalid command format");
        } else if (motor_number >= AXIS_COUNT) {
            respond(response_channel, use_checksum, "invalid motor %u", motor_number);
        } else {
            if (numscan < 3)
                current_feed_forward = 0.0f;
            Axis* axis = axes[motor_number];
            axis->controller_.set_vel_setpoint(vel_setpoint, current_feed_forward);
            axis->watchdog_feed();
        }

    } else if (cmd[0] == 'c') { // current control
        unsigned motor_number;
        float current_setpoint;
        int numscan = sscanf(cmd, "c %u %f", &motor_number, &current_setpoint);
        if (numscan < 2) {
            respond(response_channel, use_checksum, "invalid command format");
        } else if (motor_number >= AXIS_COUNT) {
            respond(response_channel, use_checksum, "invalid motor %u", motor_number);
        } else {
            Axis* axis = axes[motor_number];
            axis->controller_.set_current_setpoint(current_setpoint);
            axis->watchdog_feed();
        }

    }else if (cmd[0] == 'P') { // coupled control
        float theta_sp, gamma_sp;
        int result = parse_dual_current(cmd,len,theta_sp, gamma_sp);

        const float MULTIPLIER = 1000.0;

        theta_sp /= MULTIPLIER;
        gamma_sp /= MULTIPLIER;

        if (result != 1) {
            respond(response_channel, use_checksum, "Failed on parse or checksum: ");
            respond(response_channel, use_checksum, cmd);
        } else {
            axes[0]->controller_.set_coupled_setpoints(theta_sp, gamma_sp);
            axes[1]->controller_.set_coupled_setpoints(theta_sp, gamma_sp);

            send_motor_positions(response_channel);
        }
    } else if (cmd[0] == 'S') { // coupled control with gains
        float sp_theta, kp_theta, kd_theta;
        float sp_gamma, kp_gamma, kd_gamma;

        int result = parse_coupled_command(cmd, len, sp_theta, kp_theta, kd_theta, sp_gamma, kp_gamma, kd_gamma);

        if (result != 1) {
            respond(response_channel, use_checksum, "Failed to parse coupled command: ");
            respond(response_channel, use_checksum, cmd);
        } else {
            axes[0]->controller_.set_coupled_setpoints(sp_theta, sp_gamma);
            axes[0]->controller_.set_coupled_gains(kp_theta, kd_theta, kp_gamma, kd_gamma);

            axes[1]->controller_.set_coupled_setpoints(sp_theta, sp_gamma);
            axes[1]->controller_.set_coupled_gains(kp_theta, kd_theta, kp_gamma, kd_gamma);

            send_motor_positions(response_channel);
        }
    }  else if (cmd[0] == 't') { // trapezoidal trajectory
        unsigned motor_number;
        float goal_point;
        int numscan = sscanf(cmd, "t %u %f", &motor_number, &goal_point);
        if (numscan < 2) {
            respond(response_channel, use_checksum, "invalid command format");
        } else if (motor_number >= AXIS_COUNT) {
            respond(response_channel, use_checksum, "invalid motor %u", motor_number);
        } else {
            Axis* axis = axes[motor_number];
            axis->controller_.move_to_pos(goal_point);
            axis->watchdog_feed();
        }

    } else if (cmd[0] == 'f') { // feedback
        unsigned motor_number;
        int numscan = sscanf(cmd, "f %u", &motor_number);
        if (numscan < 1) {
            respond(response_channel, use_checksum, "invalid command format");
        } else if (motor_number >= AXIS_COUNT) {
            respond(response_channel, use_checksum, "invalid motor %u", motor_number);
        } else {
            respond(response_channel, use_checksum, "%f %f",
                    (double)axes[motor_number]->encoder_.pos_estimate_,
                    (double)axes[motor_number]->encoder_.vel_estimate_);
        }

    } else if (cmd[0] == 'h') {  // Help
        respond(response_channel, use_checksum, "Please see documentation for more details");
        respond(response_channel, use_checksum, "");
        respond(response_channel, use_checksum, "Available commands syntax reference:");
        respond(response_channel, use_checksum, "Device Info: i");
        respond(response_channel, use_checksum, "Position: q axis pos vel-lim I-lim");
        respond(response_channel, use_checksum, "Position: p axis pos vel-ff I-ff");
        respond(response_channel, use_checksum, "Velocity: v axis vel I-ff");
        respond(response_channel, use_checksum, "Current: c axis I");
        respond(response_channel,use_checksum,"Current to both motors with response: C I0 I1");
        respond(response_channel, use_checksum, "");
        respond(response_channel, use_checksum, "Properties start at odrive root, such as axis0.requested_state");
        respond(response_channel, use_checksum, "Read: r property");
        respond(response_channel, use_checksum, "Write: w property value");
        respond(response_channel, use_checksum, "");
        respond(response_channel, use_checksum, "Save config: ss");
        respond(response_channel, use_checksum, "Erase config: se");
        respond(response_channel, use_checksum, "Reboot: sr");

    } else if (cmd[0] == 'i'){ // Dump device info
        // respond(response_channel, use_checksum, "Signature: %#x", STM_ID_GetSignature());
        // respond(response_channel, use_checksum, "Revision: %#x", STM_ID_GetRevision());
        // respond(response_channel, use_checksum, "Flash Size: %#x KiB", STM_ID_GetFlashSize());
        respond(response_channel, use_checksum, "Hardware version: %d.%d-%dV", HW_VERSION_MAJOR, HW_VERSION_MINOR, HW_VERSION_VOLTAGE);
        respond(response_channel, use_checksum, "Firmware version: %d.%d.%d", FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_REVISION);
        respond(response_channel, use_checksum, "Serial number: %s", serial_number_str);

    } else if (cmd[0] == 's'){ // System
        save_configuration();

    } else if (cmd[0] == 'r') { // read property
        char name[MAX_LINE_LENGTH];
        int numscan = sscanf(cmd, "r %" TO_STR(MAX_LINE_LENGTH) "s", name);
        if (numscan < 1) {
            respond(response_channel, use_checksum, "invalid command format");
        } else {
            Endpoint* endpoint = application_endpoints_->get_by_name(name, sizeof(name));
            if (!endpoint) {
                respond(response_channel, use_checksum, "invalid property");
            } else {
                char response[10];
                bool success = endpoint->get_string(response, sizeof(response));
                if (!success)
                    respond(response_channel, use_checksum, "not implemented");
                else
                    respond(response_channel, use_checksum, response);
            }
        }

    } else if (cmd[0] == 'w') { // write property
        char name[MAX_LINE_LENGTH];
        char value[MAX_LINE_LENGTH];
        int numscan = sscanf(cmd, "w %" TO_STR(MAX_LINE_LENGTH) "s %" TO_STR(MAX_LINE_LENGTH) "s", name, value);
        if (numscan < 1) {
            respond(response_channel, use_checksum, "invalid command format");
        } else {
            Endpoint* endpoint = application_endpoints_->get_by_name(name, sizeof(name));
            if (!endpoint) {
                respond(response_channel, use_checksum, "invalid property");
            } else {
                bool success = endpoint->set_string(value, sizeof(value));
                if (!success)
                    respond(response_channel, use_checksum, "not implemented");
            }
        }

    }else if (cmd[0] == 'u') { // Update axis watchdog. 
        unsigned motor_number;
        int numscan = sscanf(cmd, "u %u", &motor_number);
        if(numscan < 1){
            respond(response_channel, use_checksum, "invalid command format");
        } else if (motor_number >= AXIS_COUNT) {
            respond(response_channel, use_checksum, "invalid motor %u", motor_number);
        }else {
            axes[motor_number]->watchdog_feed();
        }

    } else if (cmd[0] != 0) {
        respond(response_channel, use_checksum, "unknown command");
    }
}
enum RXState { IDLING, READ_LEN, READ_PAYLOAD, READ_PAYLOAD_UNTIL_NL};
void ASCII_protocol_parse_stream(const uint8_t* buffer, size_t len, StreamSink& response_channel) {
    static uint8_t parse_buffer[MAX_LINE_LENGTH];

    static uint32_t parse_buffer_idx = 0;
    static size_t payload_length = 0;
    static RXState rx_state = IDLING;
    const uint8_t START_BYTE = 1;

    while (len--) {
        // Fetch the next char
        uint8_t c = *(buffer++);

        switch(rx_state) {
            case IDLING: // wait for start byte to be received
                if(c == START_BYTE) {
                    rx_state = READ_LEN;
                }
                break;

            case READ_LEN: // use the incoming byte as payload_length
                payload_length = c; // implicitly casting uint8_t to size_t

                // If the payload is too big, probably a misread, and send
                // the receiver back to looking for the start byte
                if (payload_length >= MAX_LINE_LENGTH) {
                    rx_state = IDLING;
                } else if (payload_length == 0) {
                    rx_state = READ_PAYLOAD_UNTIL_NL;
                } else {
                    rx_state = READ_PAYLOAD;
                }
                break;

            case READ_PAYLOAD_UNTIL_NL: // read newline-terminated message
                parse_buffer[parse_buffer_idx++] = c; // store data in buffer

                if (c == '\n') { // check for stop character, aka newline
                    ASCII_protocol_process_line(parse_buffer, parse_buffer_idx, response_channel);
                    rx_state = IDLING;
                    parse_buffer_idx = 0;
                    payload_length = 0;
                }
                break;

            case READ_PAYLOAD: // read message with a fixed payload length
                parse_buffer[parse_buffer_idx++] = c; // store data in buffer

                // If we read all the data, reset the read cycle
                if (parse_buffer_idx == payload_length) {
                    // send complete payload to line processor
                    ASCII_protocol_process_line(parse_buffer, parse_buffer_idx, response_channel);
                    rx_state = IDLING;
                    parse_buffer_idx = 0;
                    payload_length = 0;
                }
                break;
        }
    }
}
