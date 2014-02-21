#ifndef TRANSMITTER_DATA_H
#define TRANSMITTER_DATA_H

typedef struct {
	char pin;
	unsigned long code;
	unsigned int period_usec;
	int repeats;
} transmit_data;

#endif

