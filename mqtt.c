/* Copyright (c) 2017 Unwired Devices LLC [info@unwds.com]
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <mosquitto.h>
#include <pthread.h>
#include <time.h>

#include <errno.h>
#include <fcntl.h> 
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/msg.h>
#include <sys/queue.h>
#include <sys/time.h>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "mqtt.h"
#include "unwds-mqtt.h"
#include "utils.h"

#define VERSION "2.2.2"

#define MAX_PENDING_NODES 1000

#define INVITE_TIMEOUT_S 45

#define NUM_RETRIES 5
#define NUM_RETRIES_INV 5

#define UART_POLLING_INTERVAL 100	// milliseconds
#define QUEUE_POLLING_INTERVAL 1 	// milliseconds
#define REPLY_LEN 1024

int msgqid;
extern int errno;

struct msg_buf {
  long mtype;
  char mtext[REPLY_LEN];
} msg_rx;

static struct mosquitto *mosq = NULL;
static int uart = 0;

static pthread_t publisher_thread;
static pthread_t reader_thread;

static pthread_mutex_t mutex_uart;

static uint8_t mqtt_format;
static int tx_delay;
static int tx_maxretr;

char logbuf[1024];
/*
static uint64_t get_posix_clock_time ()
{
    struct timespec ts;

    if (clock_gettime (CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t) (ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
    else
        return 0;
}
*/
static int set_interface_attribs (int fd, int speed, int parity)
{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
                fprintf(stderr, "error %d from tcgetattr", errno);
                return -1;
        }

        cfsetospeed (&tty, speed);
        cfsetispeed (&tty, speed);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
        // disable IGNBRK for mismatched speed tests; otherwise receive break
        // as \000 chars
        // tty.c_iflag &= ~IGNBRK;         // disable break processing
		tty.c_iflag |= IGNBRK;         // disable break processing
        tty.c_lflag = 0;                // no signaling chars, no echo,
                                        // no canonical processing
        tty.c_oflag = 0;                // no remapping, no delays
        tty.c_cc[VMIN]  = 0;            // read doesn't block
        tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

        tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

        tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls,
                                        // enable reading
        tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
        tty.c_cflag |= parity;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
        {
                fprintf(stderr, "error %d from tcsetattr\n", errno);
                return -1;
        }
        return 0;
}

static void set_blocking (int fd, int should_block)
{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
                fprintf(stderr, "error %d from tggetattr", errno);
                return;
        }

        tty.c_cc[VMIN]  = should_block ? 1 : 0;
        tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
                fprintf(stderr, "error %d setting term attributes", errno);
}

static void serve_reply(char *data) {
    puts("[info] Gate data received");

    /*
	if (strlen(str) > REPLY_LEN * 2) {
		puts("[error] Received too long reply from the gate");
		return;
	}
    */
    
    uint32_t data_size = *(uint32_t *)data;
    printf("Received data %" PRIu32 " bytes\n", data_size);
    data += 4;

	if (*data != UDUP_V5_MAGIC_BYTE) {
        printf("Magic byte mismatch, got 0x%x\n", *data);
        return;
    } else {
        puts("Magic byte matches");
    }
    
    data += 1;
    if (*data != UDUP_V5_PROTOCOL_VERSION) {
        printf("Protocol version mismatch, got 0x%x\n", *data);
        return;
    } else {
        puts("Protocol version matches");
    }
    data += 1;
    
    uint8_t message_type = *data;
    printf("Message type 0x%x\n", message_type);
    data += 1;
    
    uint64_t addr;
    memcpy(&addr, data, 8);
    if (!is_big_endian()) {
        uint64_swap_bytes(&addr);
    }
    printf("EUI64 0x%016" PRIx64 "\n", addr);
    data += 8;
    
    uint8_t voltage_tmp = *data;
    int voltage = 2000 + ((int)voltage_tmp * 50);
    printf("Voltage 0x%x\n", voltage);
    data += 1;
    
    uint8_t rssi_tmp = *data;
    int rssi = ((int)rssi_tmp - 200);
    printf("RSSI %d\n", rssi);
    data += 1;
    
    uint16_t payload_length;
    memcpy(&payload_length, data, 2);
    /* swap bytes */
    if (!is_big_endian()) {
        uint16_swap_bytes(&payload_length);
    }
    printf("Length 0x%x\n", payload_length);
    data += 2;
    
    uint8_t *payload;
    payload = malloc(payload_length);
    memcpy(payload, data, payload_length);
    
    data += payload_length;
    
    //uint16_t crc16 = *(uint16_t *)data;

	switch (message_type) {
		case UDUP_V4_COMMAND_TYPE_NET_PACKET: {
            puts("[info] Reply type: data packet");

			uint8_t modid = payload[0];
			uint8_t *moddata = payload + 1;

			char *topic = (char *)malloc(64);
			char *msg = (char *)malloc(MQTT_MAX_MSG_SIZE);
            
            mqtt_msg_t *mqtt_msg = (mqtt_msg_t *)malloc(MQTT_MSG_MAX_NUM * sizeof(mqtt_msg_t));
            memset((void *)mqtt_msg, 0, MQTT_MSG_MAX_NUM * sizeof(mqtt_msg_t));

            mqtt_status_t mqtt_status;           
            mqtt_status.rssi = rssi;
            mqtt_status.battery = voltage;
            mqtt_status.temperature = 0;

            if (modid == UNWDS_MODULE_NOT_FOUND) {
                strcpy(topic, "device");
                strcat(mqtt_msg[0].name, "error");
                char mqtt_val[50];
                snprintf(mqtt_val, 50, "module ID %d is not available", moddata[0]);
                strcat(mqtt_msg[0].value, mqtt_val);
            } else {
                if (!convert_to(modid, moddata, payload_length, topic, mqtt_msg)) {
                    snprintf(logbuf, sizeof(logbuf), "[error] Unable to convert gate reply for module %d\n", modid);
                    logprint(logbuf);
                    return;
                }
            }
            
            char addr_str[50];
            snprintf(addr_str, sizeof(addr_str), "%016" PRIx64, addr);
            
            build_mqtt_message(msg, mqtt_msg, mqtt_status, addr_str);           
            publish_mqtt_message(mosq, addr_str, topic, msg, (mqtt_format_t) mqtt_format);
            free(topic);
            free(msg);
            free(mqtt_msg);
		}
		break;

        default:
            puts("[error] Reply type: unknown reply type");
            break;
	}
}

/* Publishes messages into MQTT */
static void *publisher(void *arg)
{ 
	while(1) {
        /* Wait for a message to arrive */
        if (msgrcv(msgqid, &msg_rx, sizeof(msg_rx.mtext), 0, 0) < 0) {
            puts("[error] Failed to receive internal message");
            continue;
        }
		serve_reply(msg_rx.mtext);
	}	
	
	return NULL;
}

/* Periodic read data from UART */
static void *uart_reader(void *arg)
{
	puts("[gate] UART reading thread created");
    
    /*
    uint64_t time_last;
    uint64_t time_now;
    uint64_t time_diff;
    
    time_last = get_posix_clock_time();
    if (!time_last) {
        puts("Monotonic clock is not available");
        exit(EXIT_FAILURE);
    }
    */
    
    uint32_t num_bytes_read = 0;
    char buf[REPLY_LEN] = { '\0', };
    uint8_t bytebuf[REPLY_LEN/2];
    char c;
    int r = 0;
    
	while(1) {
        if ((r = read(uart, &c, 1)) != 0 ) {
            if (c == '\n') {
                buf[num_bytes_read] = 0;
                printf("Received: %s\n", buf);
                puts("Creating internal message");
                    
                /* convert hex to binary */
                
                hex_to_bytes(buf, bytebuf, false);
                
                num_bytes_read /= 2;
                
                msg_rx.mtype = 1;
                memcpy(msg_rx.mtext, (void *)&num_bytes_read, sizeof(num_bytes_read));
                
                memcpy(msg_rx.mtext + sizeof(num_bytes_read), bytebuf, num_bytes_read);
                
                puts("Sending internal message");
                
                if (msgsnd(msgqid, &msg_rx, sizeof(msg_rx.mtext), 0) < 0) {
                    perror( strerror(errno) );
                    printf("[error] Failed to send internal message");
                    continue;
                }
                
                num_bytes_read = 0;
            } else {
                buf[num_bytes_read++] = c;
    //            time_last = get_posix_clock_time();
                if (num_bytes_read >= REPLY_LEN) {
                    num_bytes_read = 0;
                }
            }
        }
	}
	return NULL;
}

static void message_to_mote(uint64_t addr, char *payload) 
{
	snprintf(logbuf, sizeof(logbuf), "[gate] Sending individual message to the mote with address \"%" PRIx64 "\": \"%s\"\n", 
					addr, payload);	
	logprint(logbuf);
    
    uint16_t size = strlen(payload)/2;
    uint8_t buf[140];
    uint8_t data[16] = { 0 };

    if (!hex_to_bytes(payload, data, false)) {
        puts("Error converting message from hex to bin");
        return;
    }
    
    buf[0] = UDUP_V5_MAGIC_BYTE;
    buf[1] = UDUP_V5_PROTOCOL_VERSION;
    buf[2] = UDUP_V5_COMMAND_TYPE_NET_PACKET;
    
    if (!is_big_endian()) {
        uint64_swap_bytes(&addr);
    }
    memcpy(&buf[3], &addr, 8);
    memcpy(&buf[13], &data, 16);
    // memset(&buf[13+size], 0, 16-size);
    
    uint16_t payload_size = size;
    if (!is_big_endian()) {
        uint16_swap_bytes(&payload_size);
    }
    memcpy(&buf[11], &payload_size, 2);

    uint16_t crc = crc16_arc(buf, size + 13);
    if (!is_big_endian()) {
        uint16_swap_bytes(&crc);
    }
    memcpy(&buf[13+size], &crc, 2);
    
    size = 13 + size + 2;
    
    char hexbuf[300];
    bytes_to_hex(buf, size, hexbuf, false);
    hexbuf[strlen(hexbuf)] = '\n';
    
    printf("HEX data: %s\n", hexbuf);
    
    if (write(uart, hexbuf, strlen(hexbuf)))
    {
        snprintf(logbuf, sizeof(logbuf), "Unsuccesful writing to UART device! Exit\n");
        logprint(logbuf);
        exit(EXIT_FAILURE);
    }
}

static void my_message_callback(struct mosquitto *m, void *userdata, const struct mosquitto_message *message)
{
    /* Ignore messages published by gate itself */
    /* Doesn't work with QoS 0 */
	if (message->mid != 0) {
		return;
    }

	char *running = strdup(message->topic), *token;
	const char *delims = "/";

	char topics[5][128] = {};
	int topic_count = 0;

	while (strlen(token = strsep(&running, delims))) {
		strcpy(topics[topic_count], token);
		topic_count++;

		if (running == NULL)
			break;
	}

	if (topic_count < 2) {
		return;
	}

	if (memcmp(topics[0], "devices", 7) != 0) {
		puts("[mqtt] Got message not from devices topic");	
		return;
	}

	if (memcmp(topics[1], "6lowpan", 4) != 0) {
		puts("[mqtt] Got message not from devices/6lowpan topic");	
		return;	
	}

	if (topic_count > 3) {
		/* Convert address */
		char *addr = topics[2];
		uint64_t nodeid = 0;
		bool is_broadcast = strcmp(addr, "*") == 0;

		if (!is_broadcast) {
			/* Not a broadcast address, parse it as hex EUI-64 address */
			if (!hex_to_bytes(addr, (uint8_t *) &nodeid, !is_big_endian())) {
				snprintf(logbuf, sizeof(logbuf), "[error] Invalid node address: %s\n", addr);
				logprint(logbuf);
				return;
			}
		}

        char *type;
        if (mqtt_sepio) {
            if (memcmp(topics[3], "mosi", 4) != 0) {
                snprintf(logbuf, sizeof(logbuf), "[warning] MQTT message ignored: direction is not MOSI");
				logprint(logbuf);
				return;
            }
            type = topics[4];
        } else {
            type = topics[3];
        }
		
		char buf[REPLY_LEN] = { 0 };
		if (!convert_from(type, (char *)message->payload, buf, REPLY_LEN)) {
			snprintf(logbuf, sizeof(logbuf), "[error] Convert failed. Unable to parse mqtt message: devices/6lowpan/%s : %s, %s\n", addr, type, (char*) message->payload);
			logprint(logbuf);
			return;
		}

		if (!strlen(buf)) {
			snprintf(logbuf, sizeof(logbuf), "[error] Buffer is empty. Unable to parse mqtt message: devices/6lowpan/%s : %s, %s\n", addr, type, (char*) message->payload);
			return;
		}
        
        message_to_mote(nodeid, buf);
	}
}

static void my_connect_callback(struct mosquitto *m, void *userdata, int result)
{
//	int i;
	if(!result){
		/* Subscribe to broker information topics on successful connect. */
		snprintf(logbuf, sizeof(logbuf), "Subscribing to %s\n", MQTT_SUBSCRIBE_TO);
		logprint(logbuf);

		mosquitto_subscribe(mosq, NULL, MQTT_SUBSCRIBE_TO, 2);
	}else{
		snprintf(logbuf, sizeof(logbuf), "Connect failed\n");
		logprint(logbuf);
	}
}

static void my_subscribe_callback(struct mosquitto *m, void *userdata, int mid, int qos_count, const int *granted_qos)
{
	int i;

	char tmpbuf[100];
	snprintf(logbuf, sizeof(logbuf), "Subscribed (mid: %d): %d", mid, granted_qos[0]);
	for(i=1; i<qos_count; i++){
		snprintf(tmpbuf, sizeof(tmpbuf), ", %d", granted_qos[i]);
		strcat(logbuf, tmpbuf);
	}
	strcat(logbuf, "\n");
	logprint(logbuf);
}

void usage(void) {
	printf("Usage: mqtt <serial>\nExample: mqtt [-ihdp] -p /dev/ttyS0\n");
	printf("  -i\tIgnore /etc/6lowpan-mqtt/mqtt.conf.\n");
	printf("  -h\tPrint this help.\n");
	printf("  -d\tFork to background.\n");
//	printf("  -r\tRetain last MQTT message.\n");
	printf("  -p <port>\tserial port device URI, e.g. /dev/ttyATH0.\n");
    printf("  -t\tUse MQTT format compatible with Tibbo system.\n");
}

int main(int argc, char *argv[])
{
	const char *host = "localhost";
	int port = 1883;
	int keepalive = 60;
    
    mqtt_qos = 1;
    mqtt_retain = false;

	openlog("mqtt", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_DAEMON);

	snprintf(logbuf, sizeof(logbuf), "=== MQTT gate (version: %s) ===\n", VERSION);
	logprint(logbuf);
	
    mqtt_format = UNWDS_MQTT_REGULAR;
    tx_delay = 10;
    tx_maxretr = 3;
    
	bool daemonize = 0;
//	bool retain = 0;
	char serialport[100];
	bool ignoreconfig = 0;
	
	int c;
	while ((c = getopt (argc, argv, "ihdrpt:")) != -1)
    switch (c) {
		case 'd':
			daemonize = 1;
			break;
		case 'i':
			ignoreconfig = 1;
			break;
		case 'h':
			usage();
			return 0;
			break;
        case 't':
            mqtt_format = UNWDS_MQTT_ESCAPED;
//		case 'r':
//			retain = 1;
//			break;
		case 'p':
			if (strlen(optarg) > 100) {
				snprintf(logbuf, sizeof(logbuf), "Error: serial device URI is too long\n");
				logprint(logbuf);
			}
			else {
				strncpy(serialport, optarg, 100);
			}
			break;
		default:
			usage();
			return -1;
    }
	
	// fork to background if needed and create pid file
    int pidfile = 0;
    if (daemonize)
    {
		snprintf(logbuf, sizeof(logbuf), "Attempting to run in the background\n");
		logprint(logbuf);
		
        if (daemon(0, 0))
        {
            snprintf(logbuf, sizeof(logbuf), "Error forking to background\n");
			logprint(logbuf);
            exit(EXIT_FAILURE);
        }
        
        char pidval[10];
        pidfile = open("/var/run/mqtt.pid", O_CREAT | O_RDWR, 0666);
        if (lockf(pidfile, F_TLOCK, 0) == -1)
        {
            exit(EXIT_FAILURE);
        }
        snprintf(pidval, sizeof(pidval), "%d\n", getpid());

        if (write(pidfile, pidval, strlen(pidval)) < 0)
        {
            snprintf(logbuf, sizeof(logbuf), "Cannot write to the pid file! Exit\n");
            logprint(logbuf);
            exit(EXIT_FAILURE);
        }
        
        sleep(30);
    }
	
    
    /* Create message queue */
    msgqid = msgget(IPC_PRIVATE, IPC_CREAT);
    if (msgqid < 0) {
        puts("Failed to create message queue");
        exit(EXIT_FAILURE);
    }
	
	FILE* config = NULL;
    char* token;
	
	if (!ignoreconfig)
        {
            config = fopen( "/etc/6lowpan-mqtt/mqtt.conf", "r" );
            if (config)
            {
                char *line = (char *)malloc(255);
                while(fgets(line, 254, config) != NULL)
                {
                    token = strtok(line, "\t =\n\r");
                    if (token != NULL && token[0] != '#')
                    {
                        if (!strcmp(token, "port"))
                        {
                            strcpy(serialport, strtok(NULL, "\t\n\r"));
                            while( (*serialport == ' ') || (*serialport == '=') )
                            {
                                memmove(serialport, serialport+1, strlen(serialport));
                            }
                        }
                        if (!strcmp(token, "format"))
                        {
                            char *format;
                            format = strtok(NULL, "\t =\n\r");
                            if (!strcmp(format, "mqtt-escaped")) {
                                mqtt_format = UNWDS_MQTT_ESCAPED;
                                puts("MQTT format: quotes escaped");
                            } else {
                                if (!strcmp(format, "mqtt")) {
                                    mqtt_format = UNWDS_MQTT_REGULAR;
                                    puts("MQTT format: regular");
                                }
                            }
                        }
                        if (!strcmp(token, "mqtt_qos")) {
                            char *qos;
                            qos = strtok(NULL, "\t =\n\r");
                            sscanf(qos, "%d", &mqtt_qos);
                            printf("MQTT QoS: %d\n", mqtt_qos);
                        }
                        if (!strcmp(token, "mqtt_retain")) {
                            char *retain;
                            retain = strtok(NULL, "\t =\n\r");
                            if (!strcmp(retain, "true")) {
                                mqtt_retain = true;
                                puts("MQTT retain messages enabled");
                            } else {
                                mqtt_retain = false;
                                puts("MQTT retain messages disabled");
                            }
                        }
                        if (!strcmp(token, "mqtt_sepio")) {
                            char *retain;
                            retain = strtok(NULL, "\t =\n\r");
                            if (!strcmp(retain, "true")) {
                                mqtt_sepio = true;
                                puts("MQTT separate in/out topics enabled");
                            } else {
                                mqtt_sepio = false;
                                puts("MQTT separate in/out topics disabled");
                            }
                        }
                        if (!strcmp(token, "tx_delay")) {
                            char *td;
                            td = strtok(NULL, "\t =\n\r");
                            sscanf(td, "%d", &tx_delay);
                            printf("LoRa TX queue delay: %d seconds\n", tx_delay);
                        }
                        if (!strcmp(token, "tx_maxretr")) {
                            char *td;
                            td = strtok(NULL, "\t =\n\r");
                            sscanf(td, "%d", &tx_maxretr);
                            printf("LoRa TX maximum retries: %d\n", tx_maxretr);
                        }
                    }
                }
                free(line);
                fclose(config);
            }
			else
			{
				snprintf(logbuf, sizeof(logbuf), "Configuration file /etc/6lowpan-mqtt/mqtt.conf not found\n");
				logprint(logbuf);
				return -1;
			}
        }

	printf("Using serial port device: %s\n", serialport);
	
	pthread_mutex_init(&mutex_uart, NULL);

    
	uart = open(serialport, O_RDWR | O_NOCTTY | O_SYNC);
	if (uart < 0)
	{
		snprintf(logbuf, sizeof(logbuf), "error %d opening %s: %s\n", errno, serialport, strerror (errno));
		logprint(logbuf);
		usage();
		return 1;
	}
	
	set_interface_attribs(uart, B115200, 0);  // set speed to 115,200 bps, 8n1 (no parity)
	set_blocking(uart, 0);                	 // set no blocking

	if(pthread_create(&reader_thread, NULL, uart_reader, NULL)) {
		snprintf(logbuf, sizeof(logbuf), "Error creating reader thread\n");
		logprint(logbuf);
		return 1;
	}

	if(pthread_create(&publisher_thread, NULL, publisher, NULL)) {
		snprintf(logbuf, sizeof(logbuf), "Error creating publisher thread\n");
		logprint(logbuf);
		return 1;
	}

	mosquitto_lib_init();
	mosq = mosquitto_new(NULL, true, NULL);
	if(!mosq){
		snprintf(logbuf, sizeof(logbuf), "Error: Out of memory.\n");
		logprint(logbuf);
		return 1;
	}
	
	mosquitto_connect_callback_set(mosq, my_connect_callback);
	mosquitto_message_callback_set(mosq, my_message_callback);
	mosquitto_subscribe_callback_set(mosq, my_subscribe_callback);

	if(mosquitto_connect(mosq, host, port, keepalive)){
		snprintf(logbuf, sizeof(logbuf), "Unable to connect.\n");
		logprint(logbuf);
		return 1;
	}

	snprintf(logbuf, sizeof(logbuf), "[mqtt] Entering event loop");
	logprint(logbuf);

	mosquitto_loop_forever(mosq, -1, 1);

	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
	
	if (pidfile)
    {
        lockf(pidfile, F_ULOCK, 0);
        close(pidfile);
        remove("/var/run/mqtt.pid");
    }
	
	syslog(LOG_INFO, "mqtt service stopped");
    closelog();
	
	return 0;
}
