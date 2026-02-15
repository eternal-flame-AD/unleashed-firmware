#include "hex.h"
#include <furi.h>
#include <furi_hal.h>
#include <lib/toolbox/args.h>
#include <toolbox/pipe.h>
#include <cli/cli_main_commands.h>
#include <toolbox/cli/cli_registry.h>

#include <ble/ble.h>
#include "bt_settings.h"
#include "bt_service/bt.h"
#include <profiles/serial_profile.h>

static void bt_cli_command_hci_info(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    UNUSED(args);
    UNUSED(context);
    FuriString* buffer;
    buffer = furi_string_alloc();
    furi_hal_bt_dump_state(buffer);
    printf("%s", furi_string_get_cstr(buffer));
    furi_string_free(buffer);
}

static void bt_cli_command_scan_callback(
    uint8_t event_type,
    uint8_t address_type,
    uint8_t mac_address[6],
    int8_t rssi,
    uint8_t size,
    const uint8_t* data,
    void* context) {
    PipeSide* pipe = context;

    char buf[256];
    snprintf(
        buf,
        sizeof(buf),
        "Scan callback: event_type = %d, address_type = %d, mac_address = %02X:%02X:%02X:%02X:%02X:%02X, rssi = %d, size = %d\r\n",
        event_type,
        address_type,
        mac_address[5],
        mac_address[4],
        mac_address[3],
        mac_address[2],
        mac_address[1],
        mac_address[0],
        rssi,
        size);
    pipe_send(pipe, buf, strlen(buf));

    uint8_t cursor = 0;
    while(cursor + 1 < size) {
        uint8_t chunk_size = data[cursor];
        if(chunk_size == 0) {
            break;
        }
        uint8_t tag = data[cursor + 1];
        const uint8_t* chunk_data = data + cursor + 2;
        chunk_size--;
        if(chunk_data + chunk_size > data + size) {
            pipe_send(pipe, " {BufferOverRun}\r\n", 18);
            break;
        }
        switch(tag) {
        case 0x01:
            if(chunk_size > 0) {
                pipe_send(pipe, "  Flags:", 7);
                uint8_t flags = chunk_data[0];
                if(flags & 1) {
                    pipe_send(pipe, " LimitedDisc", 12);
                }
                if(flags & 2) {
                    pipe_send(pipe, " GenrealDisc", 12);
                }
                if(flags & 4) {
                    pipe_send(pipe, " NBL/EDR", 8);
                }
                if(flags & 8) {
                    pipe_send(pipe, " EDR(Cl)", 8);
                }
                pipe_send(pipe, "\r\n", 2);
            }
            break;
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
            snprintf(buf, sizeof(buf), "  Service UUID: ");
            pipe_send(pipe, buf, strlen(buf));
            uint8_to_hex_chars(chunk_data, (uint8_t*)buf, chunk_size * 2);
            pipe_send(pipe, buf, chunk_size * 2);
            pipe_send(pipe, "\r\n", 2);
            break;
        case 0x16:
        case 0x20:
        case 0x21:
            snprintf(buf, sizeof(buf), "  Service Data: ");
            pipe_send(pipe, buf, strlen(buf));
            uint8_to_hex_chars(chunk_data, (uint8_t*)buf, chunk_size * 2);
            pipe_send(pipe, buf, chunk_size * 2);
            pipe_send(pipe, "\r\n", 2);
            break;
        case 0x08:
        case 0x09:
            if(tag == 0x08) {
                pipe_send(pipe, "  Shortened Local Name: ", 24);
            } else {
                pipe_send(pipe, "  Complete Local Name: ", 23);
            }
            pipe_send(pipe, buf, strlen(buf));
            pipe_send(pipe, chunk_data, chunk_size);
            pipe_send(pipe, "\r\n", 2);
            break;
        default:
            snprintf(buf, sizeof(buf), "  ADV[%02X]: {", tag);
            pipe_send(pipe, buf, strlen(buf));
            uint8_to_hex_chars(chunk_data, (uint8_t*)buf, chunk_size * 2);
            pipe_send(pipe, buf, chunk_size * 2);
            pipe_send(pipe, "}\r\n", 3);
            break;
        }
        cursor += chunk_size + 2;
    }
    pipe_send(pipe, "\r\n", 2);
}

static void bt_cli_command_scan(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    UNUSED(args);
    UNUSED(context);

    int scan_type = 1;
    if(!args_read_int_and_trim(args, &scan_type) && (scan_type < 0 || scan_type > 1)) {
        printf("Incorrect or missing scan type, expected int 0-1");
        return;
    }
    furi_hal_bt_stop_scanning();
    furi_hal_bt_start_scanning(bt_cli_command_scan_callback, pipe, scan_type & 1);
    while(furi_hal_bt_is_scanning()) {
        if(cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            break;
        }
        furi_delay_ms(100);
    }
    furi_hal_bt_stop_scanning();
}

static void bt_cli_command_carrier_tx(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    int channel = 0;
    int power = 0;

    do {
        if(!args_read_int_and_trim(args, &channel) && (channel < 0 || channel > 39)) {
            printf("Incorrect or missing channel, expected int 0-39");
            break;
        }
        if(!args_read_int_and_trim(args, &power) && (power < 0 || power > 6)) {
            printf("Incorrect or missing power, expected int 0-6");
            break;
        }

        Bt* bt = furi_record_open(RECORD_BT);
        bt_disconnect(bt);
        furi_hal_bt_reinit();
        printf("Transmitting carrier at %d channel at %d dB power\r\n", channel, power);
        printf("Press CTRL+C to stop\r\n");
        furi_hal_bt_start_tone_tx(channel, 0x19 + power);

        while(!cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            furi_delay_ms(250);
        }
        furi_hal_bt_stop_tone_tx();

        bt_profile_restore_default(bt);
        furi_record_close(RECORD_BT);
    } while(false);
}

static void bt_cli_command_carrier_rx(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    int channel = 0;

    do {
        if(!args_read_int_and_trim(args, &channel) && (channel < 0 || channel > 39)) {
            printf("Incorrect or missing channel, expected int 0-39");
            break;
        }

        Bt* bt = furi_record_open(RECORD_BT);
        bt_disconnect(bt);
        furi_hal_bt_reinit();
        printf("Receiving carrier at %d channel\r\n", channel);
        printf("Press CTRL+C to stop\r\n");

        furi_hal_bt_start_packet_rx(channel, 1);

        while(!cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            furi_delay_ms(250);
            printf("RSSI: %6.1f dB\r", (double)furi_hal_bt_get_rssi());
            fflush(stdout);
        }

        furi_hal_bt_stop_packet_test();

        bt_profile_restore_default(bt);
        furi_record_close(RECORD_BT);
    } while(false);
}

static void bt_cli_command_packet_tx(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    int channel = 0;
    int pattern = 0;
    int datarate = 1;

    do {
        if(!args_read_int_and_trim(args, &channel) && (channel < 0 || channel > 39)) {
            printf("Incorrect or missing channel, expected int 0-39");
            break;
        }
        if(!args_read_int_and_trim(args, &pattern) && (pattern < 0 || pattern > 5)) {
            printf("Incorrect or missing pattern, expected int 0-5 \r\n");
            printf("0 - Pseudo-Random bit sequence 9\r\n");
            printf("1 - Pattern of alternating bits '11110000'\r\n");
            printf("2 - Pattern of alternating bits '10101010'\r\n");
            printf("3 - Pseudo-Random bit sequence 15\r\n");
            printf("4 - Pattern of All '1' bits\r\n");
            printf("5 - Pattern of All '0' bits\r\n");
            break;
        }
        if(!args_read_int_and_trim(args, &datarate) && (datarate < 1 || datarate > 2)) {
            printf("Incorrect or missing datarate, expected int 1-2");
            break;
        }

        Bt* bt = furi_record_open(RECORD_BT);
        bt_disconnect(bt);
        furi_hal_bt_reinit();
        printf(
            "Transmitting %d pattern packet at %d channel at %d M datarate\r\n",
            pattern,
            channel,
            datarate);
        printf("Press CTRL+C to stop\r\n");
        furi_hal_bt_start_packet_tx(channel, pattern, datarate);

        while(!cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            furi_delay_ms(250);
        }
        furi_hal_bt_stop_packet_test();
        printf("Transmitted %lu packets", furi_hal_bt_get_transmitted_packets());

        bt_profile_restore_default(bt);
        furi_record_close(RECORD_BT);
    } while(false);
}

static void bt_cli_command_packet_rx(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    int channel = 0;
    int datarate = 1;

    do {
        if(!args_read_int_and_trim(args, &channel) && (channel < 0 || channel > 39)) {
            printf("Incorrect or missing channel, expected int 0-39");
            break;
        }
        if(!args_read_int_and_trim(args, &datarate) && (datarate < 1 || datarate > 2)) {
            printf("Incorrect or missing datarate, expected int 1-2");
            break;
        }

        Bt* bt = furi_record_open(RECORD_BT);
        bt_disconnect(bt);
        furi_hal_bt_reinit();
        printf("Receiving packets at %d channel at %d M datarate\r\n", channel, datarate);
        printf("Press CTRL+C to stop\r\n");
        furi_hal_bt_start_packet_rx(channel, datarate);

        while(!cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            furi_delay_ms(250);
            printf("RSSI: %03.1f dB\r", (double)furi_hal_bt_get_rssi());
            fflush(stdout);
        }
        uint16_t packets_received = furi_hal_bt_stop_packet_test();
        printf("Received %hu packets", packets_received);

        bt_profile_restore_default(bt);
        furi_record_close(RECORD_BT);
    } while(false);
}

static void bt_cli_print_usage(void) {
    printf("Usage:\r\n");
    printf("bt <cmd> <args>\r\n");
    printf("Cmd list:\r\n");
    printf("\thci_info\t - HCI info\r\n");
    printf("\tscan\t - start scanning\r\n");
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug) && furi_hal_bt_is_testing_supported()) {
        printf("\ttx_carrier <channel:0-39> <power:0-6>\t - start tx carrier test\r\n");
        printf("\trx_carrier <channel:0-39>\t - start rx carrier test\r\n");
        printf(
            "\ttx_packet <channel:0-39> <pattern:0-5> <datarate:1-2>\t - start tx packet test\r\n");
        printf("\trx_packet <channel:0-39> <datarate:1-2>\t - start rx packer test\r\n");
    }
}

static void bt_cli(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    furi_record_open(RECORD_BT);

    FuriString* cmd;
    cmd = furi_string_alloc();
    BtSettings bt_settings;
    bt_settings_load(&bt_settings);

    do {
        if(!args_read_string_and_trim(args, cmd)) {
            bt_cli_print_usage();
            break;
        }
        if(furi_string_cmp_str(cmd, "hci_info") == 0) {
            bt_cli_command_hci_info(pipe, args, NULL);
            break;
        }
        if(furi_string_cmp_str(cmd, "scan") == 0) {
            bt_cli_command_scan(pipe, args, NULL);
            break;
        }
        if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug) && furi_hal_bt_is_testing_supported()) {
            if(furi_string_cmp_str(cmd, "tx_carrier") == 0) {
                bt_cli_command_carrier_tx(pipe, args, NULL);
                break;
            }
            if(furi_string_cmp_str(cmd, "rx_carrier") == 0) {
                bt_cli_command_carrier_rx(pipe, args, NULL);
                break;
            }
            if(furi_string_cmp_str(cmd, "tx_packet") == 0) {
                bt_cli_command_packet_tx(pipe, args, NULL);
                break;
            }
            if(furi_string_cmp_str(cmd, "rx_packet") == 0) {
                bt_cli_command_packet_rx(pipe, args, NULL);
                break;
            }
        }

        bt_cli_print_usage();
    } while(false);

    if(bt_settings.enabled) {
        furi_hal_bt_start_advertising();
    }

    furi_string_free(cmd);
    furi_record_close(RECORD_BT);
}

void bt_on_system_start(void) {
#ifdef SRV_CLI
    CliRegistry* registry = furi_record_open(RECORD_CLI);
    cli_registry_add_command(registry, "bt", CliCommandFlagDefault, bt_cli, NULL);
    furi_record_close(RECORD_CLI);
#else
    UNUSED(bt_cli);
#endif
}
