#ifndef _HELPERS_H
#define _HELPERS_H 1

#include <stdio.h>
#include <stdlib.h>

/*
 * Macro de verificare a erorilor
 * Exemplu:
 *     int fd = open(file_name, O_RDONLY);
 *     DIE(fd == -1, "open failed");
 */

#define DIE(assertion, call_description)	\
	do {									\
		if (assertion) {					\
			fprintf(stderr, "(%s, %d): ",	\
					__FILE__, __LINE__);	\
			perror(call_description);		\
			exit(EXIT_FAILURE);				\
		}									\
	} while(0)

#define BUFLEN		1553	// dimensiunea maxima a calupului de date
#define TOPIC_LEN 50
#define DATATYPE_LEN 1
#define CONTENT_LEN 1501
#define MAX_CLIENTS	100	// numarul maxim de clienti in asteptare
#define MAX(a,b) (((a) > (b)) ? (a) : (b))

#endif
